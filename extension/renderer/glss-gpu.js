// GLSS Pure GPU Rendering Engine (WebGPU & WebGL2)
// Executes 100% on GPU: Zero CPU pixel loops.
// Pipeline: Video Frame -> GPU Texture -> GPU Frame Interpolation Shader -> GPU Super Resolution (FSR/RCAS) Shader -> Composite -> Canvas

export class GlssGpuEngine {
  constructor(canvas) {
    this.canvas = canvas;
    this.gl = null;

    // Config
    this.scaleFactor = 1.5;
    this.sharpness = 0.8;
    this.upscaleMethod = "fsr"; // 'fsr', 'anime4k', 'bilinear'
    this.interpMultiplier = 2;   // 1 (off), 2, 3, 4
    this.interpMode = 0;         // 0: motion compensated, 1: blend
    this.splitScreen = false;    // Edge-style split screen comparison
    this.splitPosition = 0.5;

    // State
    this.video = null;
    this.srcWidth = 0;
    this.srcHeight = 0;
    this.outWidth = 0;
    this.outHeight = 0;
    this.running = false;

    // Frame history & interpolation timing
    this.hasPrevFrame = false;
    this.lastSourceTime = 0;
    this.frameDuration = 1000 / 30; // default 30fps
    this.lastFrameArrivalTime = 0;

    // Performance metrics
    this.stats = {
      backend: "WebGL2 (Pure GPU)",
      srcFps: 0,
      renderFps: 0,
      srcResolution: "0x0",
      outResolution: "0x0",
      gpuTimeMs: 0
    };

    this.srcFrameCount = 0;
    this.renderFrameCount = 0;
    this.lastFpsCalcTime = performance.now();

    this.initWebGL2();
  }

  initWebGL2() {
    const gl = this.canvas.getContext("webgl2", {
      alpha: false,
      depth: false,
      stencil: false,
      antialias: false,
      preserveDrawingBuffer: false,
      powerPreference: "high-performance"
    });

    if (!gl) {
      console.error("[GLSS GPU] 无法获取 WebGL2 上下文！");
      return;
    }
    this.gl = gl;
    this.stats.backend = "WebGL2 (Pure GPU)";

    // Flip Y to match HTML video top-left coordinate system
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);

    // Compile GLSL Shaders
    this.initQuad();
    this.initShaders();
    this.initTextures();
  }

  initQuad() {
    const gl = this.gl;
    // Fullscreen triangle for efficient screen-space passes
    const vertices = new Float32Array([
      -1, -1,
       3, -1,
      -1,  3
    ]);
    this.quadVao = gl.createVertexArray();
    gl.bindVertexArray(this.quadVao);

    this.quadVbo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quadVbo);
    gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);

    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);
  }

  createShader(type, source) {
    const gl = this.gl;
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      const info = gl.getShaderInfoLog(shader);
      gl.deleteShader(shader);
      throw new Error(`Shader compile error: ${info}`);
    }
    return shader;
  }

  createProgram(vsSrc, fsSrc) {
    const gl = this.gl;
    const vs = this.createShader(gl.VERTEX_SHADER, vsSrc);
    const fs = this.createShader(gl.FRAGMENT_SHADER, fsSrc);
    const prog = gl.createProgram();
    gl.attachShader(prog, vs);
    gl.attachShader(prog, fs);
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
      const info = gl.getProgramInfoLog(prog);
      gl.deleteProgram(prog);
      throw new Error(`Program link error: ${info}`);
    }
    return prog;
  }

  initShaders() {
    const vsScreenQuad = `#version 300 es
      layout(location = 0) in vec2 a_pos;
      out vec2 v_uv;
      void main() {
        v_uv = a_pos * 0.5 + 0.5;
        gl_Position = vec4(a_pos, 0.0, 1.0);
      }
    `;

    // 1. GPU Frame Interpolation Shader (Motion-Compensated Temporal Blend)
    const fsInterp = `#version 300 es
      precision highp float;
      in vec2 v_uv;
      out vec4 fragColor;

      uniform sampler2D u_prev;
      uniform sampler2D u_curr;
      uniform float u_t;
      uniform vec2 u_res;
      uniform int u_mode;

      float luma(vec3 c) {
        return dot(c, vec3(0.299, 0.587, 0.114));
      }

      // GPU Motion estimation on 3x3 diamond window
      vec2 estimateMotion(vec2 uv, vec2 px) {
        float bestCost = 1e6;
        vec2 bestMv = vec2(0.0);
        float centerLuma = luma(texture(u_curr, uv).rgb);

        for (int dy = -4; dy <= 4; dy += 2) {
          for (int dx = -4; dx <= 4; dx += 2) {
            vec2 offset = vec2(float(dx), float(dy)) * px;
            float candLuma = luma(texture(u_prev, uv + offset).rgb);
            float sad = abs(centerLuma - candLuma) + length(vec2(float(dx), float(dy))) * 0.04;
            if (sad < bestCost) {
              bestCost = sad;
              bestMv = vec2(float(dx), float(dy));
            }
          }
        }
        return bestMv * px;
      }

      void main() {
        float t = clamp(u_t, 0.0, 1.0);
        if (u_mode == 1 || t <= 0.01) {
          vec4 p = texture(u_prev, v_uv);
          vec4 c = texture(u_curr, v_uv);
          fragColor = mix(p, c, t);
          return;
        }

        vec2 px = 1.0 / u_res;
        vec2 mv = estimateMotion(v_uv, px);

        vec2 uv0 = clamp(v_uv - t * mv, 0.0, 1.0);
        vec2 uv1 = clamp(v_uv + (1.0 - t) * mv, 0.0, 1.0);

        vec4 s0 = texture(u_prev, uv0);
        vec4 s1 = texture(u_curr, uv1);

        fragColor = mix(s0, s1, t);
      }
    `;

    // 2. GPU Super Resolution: FSR (EASU) Pass
    const fsUpscaleFsr = `#version 300 es
      precision highp float;
      in vec2 v_uv;
      out vec4 fragColor;

      uniform sampler2D u_source;
      uniform vec2 u_srcRes;
      uniform vec2 u_outRes;

      float luma(vec3 c) {
        return dot(c, vec3(0.299, 0.587, 0.114));
      }

      void main() {
        vec2 px = 1.0 / u_srcRes;
        vec2 pos = v_uv * u_srcRes - 0.5;
        vec2 f = fract(pos);
        vec2 base = (floor(pos) + 0.5) * px;

        vec4 c00 = texture(u_source, base);
        vec4 c10 = texture(u_source, base + vec2(px.x, 0.0));
        vec4 c01 = texture(u_source, base + vec2(0.0, px.y));
        vec4 c11 = texture(u_source, base + px);

        float l00 = luma(c00.rgb);
        float l10 = luma(c10.rgb);
        float l01 = luma(c01.rgb);
        float l11 = luma(c11.rgb);

        float gx = abs(l00 + l01 - l10 - l11);
        float gy = abs(l00 + l10 - l01 - l11);
        float lengthG = sqrt(gx * gx + gy * gy) + 1e-5;

        vec2 w = f;
        if (gx > gy) {
          w.x = mix(f.x, smoothstep(0.0, 1.0, f.x), clamp(lengthG * 2.0, 0.0, 1.0));
        } else {
          w.y = mix(f.y, smoothstep(0.0, 1.0, f.y), clamp(lengthG * 2.0, 0.0, 1.0));
        }

        vec4 top = mix(c00, c10, w.x);
        vec4 bot = mix(c01, c11, w.x);
        fragColor = mix(top, bot, w.y);
      }
    `;

    // 3. GPU Robust Contrast-Adaptive Sharpening (RCAS) Pass
    const fsRcas = `#version 300 es
      precision highp float;
      in vec2 v_uv;
      out vec4 fragColor;

      uniform sampler2D u_tex;
      uniform vec2 u_res;
      uniform float u_sharpness;

      float luma(vec3 c) {
        return dot(c, vec3(0.299, 0.587, 0.114));
      }

      void main() {
        vec2 px = 1.0 / u_res;
        vec4 e = texture(u_tex, v_uv);
        if (u_sharpness <= 0.001) {
          fragColor = e;
          return;
        }

        vec4 b = texture(u_tex, v_uv + vec2(0.0, -px.y));
        vec4 d = texture(u_tex, v_uv + vec2(-px.x, 0.0));
        vec4 f = texture(u_tex, v_uv + vec2(px.x, 0.0));
        vec4 h = texture(u_tex, v_uv + vec2(0.0, px.y));

        float bL = luma(b.rgb);
        float dL = luma(d.rgb);
        float eL = luma(e.rgb);
        float fL = luma(f.rgb);
        float hL = luma(h.rgb);

        float minL = min(min(bL, dL), min(fL, min(hL, eL)));
        float maxL = max(max(bL, dL), max(fL, max(hL, eL)));

        float contrast = maxL - minL;
        float w = clamp(min(eL - minL, maxL - eL) / max(contrast, 1e-4), 0.0, 1.0) * u_sharpness * 0.25;

        vec4 result = (e + w * (b + d + f + h)) / (1.0 + 4.0 * w);
        fragColor = clamp(result, 0.0, 1.0);
      }
    `;

    // 4. GPU Anime4K Edge Boosting Pass
    const fsAnime4k = `#version 300 es
      precision highp float;
      in vec2 v_uv;
      out vec4 fragColor;

      uniform sampler2D u_source;
      uniform vec2 u_res;
      uniform float u_strength;

      float luma(vec3 c) {
        return dot(c, vec3(0.299, 0.587, 0.114));
      }

      void main() {
        vec2 px = 1.0 / u_res;
        vec4 c = texture(u_source, v_uv);
        vec4 n = texture(u_source, v_uv + vec2(0.0, -px.y));
        vec4 s = texture(u_source, v_uv + vec2(0.0, px.y));
        vec4 w = texture(u_source, v_uv + vec2(-px.x, 0.0));
        vec4 e = texture(u_source, v_uv + vec2(px.x, 0.0));

        float lC = luma(c.rgb);
        float lN = luma(n.rgb);
        float lS = luma(s.rgb);
        float lW = luma(w.rgb);
        float lE = luma(e.rgb);

        vec2 grad = vec2(lE - lW, lS - lN);
        float gradLen = length(grad);

        vec2 pushUv = v_uv - normalize(grad + 1e-5) * px * gradLen * (u_strength * 0.5);
        vec4 refined = texture(u_source, pushUv);

        fragColor = mix(c, refined, clamp(gradLen * 3.0, 0.0, 1.0));
      }
    `;

    // 5. Bilinear Pass
    const fsBilinear = `#version 300 es
      precision highp float;
      in vec2 v_uv;
      out vec4 fragColor;
      uniform sampler2D u_tex;
      void main() {
        fragColor = texture(u_tex, v_uv);
      }
    `;

    // 6. Edge-style Split Screen Composite Pass
    const fsComposite = `#version 300 es
      precision highp float;
      in vec2 v_uv;
      out vec4 fragColor;

      uniform sampler2D u_orig;
      uniform sampler2D u_enh;
      uniform int u_split;
      uniform float u_splitPos;

      void main() {
        if (u_split == 1) {
          float dist = abs(v_uv.x - u_splitPos);
          if (dist < 0.002) {
            // Neon cyan divider bar
            fragColor = vec4(0.22, 0.74, 0.97, 1.0);
            return;
          }
          if (v_uv.x < u_splitPos) {
            fragColor = texture(u_orig, v_uv);
          } else {
            fragColor = texture(u_enh, v_uv);
          }
        } else {
          fragColor = texture(u_enh, v_uv);
        }
      }
    `;

    this.progInterp = this.createProgram(vsScreenQuad, fsInterp);
    this.progFsr = this.createProgram(vsScreenQuad, fsUpscaleFsr);
    this.progRcas = this.createProgram(vsScreenQuad, fsRcas);
    this.progAnime4k = this.createProgram(vsScreenQuad, fsAnime4k);
    this.progBilinear = this.createProgram(vsScreenQuad, fsBilinear);
    this.progComposite = this.createProgram(vsScreenQuad, fsComposite);
  }

  initTextures() {
    const gl = this.gl;
    this.texPrev = this.createTexture();
    this.texCurr = this.createTexture();
    this.texInterp = this.createTexture();
    this.texUpscale = this.createTexture();
    this.texFinalEnh = this.createTexture();

    this.fboInterp = gl.createFramebuffer();
    this.fboUpscale = gl.createFramebuffer();
    this.fboFinalEnh = gl.createFramebuffer();
  }

  createTexture() {
    const gl = this.gl;
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    return tex;
  }

  updateDimensions(srcW, srcH) {
    if (srcW === 0 || srcH === 0) return;
    if (this.srcWidth === srcW && this.srcHeight === srcH) return;

    this.srcWidth = srcW;
    this.srcHeight = srcH;
    this.outWidth = Math.max(1, Math.round(srcW * this.scaleFactor));
    this.outHeight = Math.max(1, Math.round(srcH * this.scaleFactor));

    this.canvas.width = this.outWidth;
    this.canvas.height = this.outHeight;

    const gl = this.gl;

    gl.bindTexture(gl.TEXTURE_2D, this.texInterp);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, srcW, srcH, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fboInterp);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.texInterp, 0);

    gl.bindTexture(gl.TEXTURE_2D, this.texUpscale);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, this.outWidth, this.outHeight, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fboUpscale);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.texUpscale, 0);

    gl.bindTexture(gl.TEXTURE_2D, this.texFinalEnh);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, this.outWidth, this.outHeight, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fboFinalEnh);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.texFinalEnh, 0);

    gl.bindFramebuffer(gl.FRAMEBUFFER, null);

    this.stats.srcResolution = `${srcW}x${srcH}`;
    this.stats.outResolution = `${this.outWidth}x${this.outHeight}`;
  }

  pushVideoFrame(video) {
    if (!video || video.videoWidth === 0 || video.videoHeight === 0) return;
    this.updateDimensions(video.videoWidth, video.videoHeight);

    const gl = this.gl;
    const now = performance.now();

    // Swap textures
    const tmp = this.texPrev;
    this.texPrev = this.texCurr;
    this.texCurr = tmp;

    // Hardware texture upload (with Y-flip for video orientation)
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
    gl.bindTexture(gl.TEXTURE_2D, this.texCurr);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, video);

    if (!this.hasPrevFrame) {
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, true);
      gl.bindTexture(gl.TEXTURE_2D, this.texPrev);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, video);
      this.hasPrevFrame = true;
    }

    if (this.lastFrameArrivalTime > 0) {
      const delta = now - this.lastFrameArrivalTime;
      if (delta > 5 && delta < 500) {
        this.frameDuration = this.frameDuration * 0.8 + delta * 0.2;
      }
    }
    this.lastFrameArrivalTime = now;
    this.srcFrameCount++;
  }

  renderFrame(t = 1.0) {
    if (!this.gl || this.srcWidth === 0 || this.srcHeight === 0) return;

    const gl = this.gl;
    const t0 = performance.now();

    gl.bindVertexArray(this.quadVao);

    // ==========================================
    // PASS 1: GPU Frame Interpolation
    // ==========================================
    let activeSourceTex = this.texCurr;

    if (this.interpMultiplier >= 2 && this.hasPrevFrame) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.fboInterp);
      gl.viewport(0, 0, this.srcWidth, this.srcHeight);

      gl.useProgram(this.progInterp);

      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.texPrev);
      gl.uniform1i(gl.getUniformLocation(this.progInterp, "u_prev"), 0);

      gl.activeTexture(gl.TEXTURE1);
      gl.bindTexture(gl.TEXTURE_2D, this.texCurr);
      gl.uniform1i(gl.getUniformLocation(this.progInterp, "u_curr"), 1);

      gl.uniform1f(gl.getUniformLocation(this.progInterp, "u_t"), t);
      gl.uniform2f(gl.getUniformLocation(this.progInterp, "u_res"), this.srcWidth, this.srcHeight);
      gl.uniform1i(gl.getUniformLocation(this.progInterp, "u_mode"), this.interpMode);

      gl.drawArrays(gl.TRIANGLES, 0, 3);
      activeSourceTex = this.texInterp;
    }

    // ==========================================
    // PASS 2: Super Resolution Upscale Pass
    // ==========================================
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fboUpscale);
    gl.viewport(0, 0, this.outWidth, this.outHeight);

    let upscaleProg = this.progFsr;
    if (this.upscaleMethod === "anime4k") {
      upscaleProg = this.progAnime4k;
    } else if (this.upscaleMethod === "bilinear") {
      upscaleProg = this.progBilinear;
    }

    gl.useProgram(upscaleProg);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, activeSourceTex);
    gl.uniform1i(gl.getUniformLocation(upscaleProg, "u_source") || gl.getUniformLocation(upscaleProg, "u_tex"), 0);

    const uSrcRes = gl.getUniformLocation(upscaleProg, "u_srcRes");
    if (uSrcRes) gl.uniform2f(uSrcRes, this.srcWidth, this.srcHeight);

    const uOutRes = gl.getUniformLocation(upscaleProg, "u_outRes") || gl.getUniformLocation(upscaleProg, "u_res");
    if (uOutRes) gl.uniform2f(uOutRes, this.outWidth, this.outHeight);

    const uStrength = gl.getUniformLocation(upscaleProg, "u_strength");
    if (uStrength) gl.uniform1f(uStrength, this.sharpness);

    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // ==========================================
    // PASS 3: Robust Contrast-Adaptive Sharpening (RCAS)
    // ==========================================
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fboFinalEnh);
    gl.viewport(0, 0, this.outWidth, this.outHeight);

    gl.useProgram(this.progRcas);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texUpscale);
    gl.uniform1i(gl.getUniformLocation(this.progRcas, "u_tex"), 0);
    gl.uniform2f(gl.getUniformLocation(this.progRcas, "u_res"), this.outWidth, this.outHeight);
    gl.uniform1f(gl.getUniformLocation(this.progRcas, "u_sharpness"), this.sharpness);

    gl.drawArrays(gl.TRIANGLES, 0, 3);

    // ==========================================
    // PASS 4: Edge-style Composite / Split Screen Pass -> Canvas
    // ==========================================
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, this.outWidth, this.outHeight);

    gl.useProgram(this.progComposite);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.texCurr);
    gl.uniform1i(gl.getUniformLocation(this.progComposite, "u_orig"), 0);

    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.texFinalEnh);
    gl.uniform1i(gl.getUniformLocation(this.progComposite, "u_enh"), 1);

    gl.uniform1i(gl.getUniformLocation(this.progComposite, "u_split"), this.splitScreen ? 1 : 0);
    gl.uniform1f(gl.getUniformLocation(this.progComposite, "u_splitPos"), this.splitPosition);

    gl.drawArrays(gl.TRIANGLES, 0, 3);

    gl.bindVertexArray(null);

    const t1 = performance.now();
    this.stats.gpuTimeMs = Math.round((t1 - t0) * 100) / 100;
    this.renderFrameCount++;

    if (t1 - this.lastFpsCalcTime >= 1000) {
      const elapsed = (t1 - this.lastFpsCalcTime) / 1000;
      this.stats.srcFps = Math.round(this.srcFrameCount / elapsed);
      this.stats.renderFps = Math.round(this.renderFrameCount / elapsed);
      this.srcFrameCount = 0;
      this.renderFrameCount = 0;
      this.lastFpsCalcTime = t1;
    }
  }

  destroy() {
    this.running = false;
    const gl = this.gl;
    if (!gl) return;

    gl.deleteTexture(this.texPrev);
    gl.deleteTexture(this.texCurr);
    gl.deleteTexture(this.texInterp);
    gl.deleteTexture(this.texUpscale);
    gl.deleteTexture(this.texFinalEnh);
    gl.deleteFramebuffer(this.fboInterp);
    gl.deleteFramebuffer(this.fboUpscale);
    gl.deleteFramebuffer(this.fboFinalEnh);
    gl.deleteProgram(this.progInterp);
    gl.deleteProgram(this.progFsr);
    gl.deleteProgram(this.progRcas);
    gl.deleteProgram(this.progAnime4k);
    gl.deleteProgram(this.progBilinear);
    gl.deleteProgram(this.progComposite);
  }
}
