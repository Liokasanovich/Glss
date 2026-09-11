// Demo video stream generator
// Renders a high-contrast 24 FPS animation to an offscreen canvas and streams it to <video>

const video = document.getElementById("demo-video");

const canvas = document.createElement("canvas");
canvas.width = 960;
canvas.height = 540;
const ctx = canvas.getContext("2d");

// Stream canvas at 24 FPS (film cadence)
const stream = canvas.captureStream(24);
video.srcObject = stream;
video.play().catch(() => {});

// Animation state
let frameIdx = 0;
const balls = [
  { x: 100, y: 150, vx: 5, vy: 4, r: 28, color: "#f43f5e" },
  { x: 300, y: 300, vx: -6, vy: 5, r: 35, color: "#38bdf8" },
  { x: 500, y: 200, vx: 4, vy: -6, r: 24, color: "#a855f7" },
  { x: 700, y: 400, vx: -5, vy: -4, r: 40, color: "#34d399" }
];

function drawDemoFrame() {
  frameIdx++;

  // Clear background
  ctx.fillStyle = "#0c1322";
  ctx.fillRect(0, 0, canvas.width, canvas.height);

  // Draw motion grid
  ctx.strokeStyle = "rgba(255, 255, 255, 0.05)";
  ctx.lineWidth = 1;
  const gridSize = 40;
  for (let x = 0; x < canvas.width; x += gridSize) {
    ctx.beginPath();
    ctx.moveTo(x, 0);
    ctx.lineTo(x, canvas.height);
    ctx.stroke();
  }
  for (let y = 0; y < canvas.height; y += gridSize) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(canvas.width, y);
    ctx.stroke();
  }

  // Draw high-frequency text patterns (ideal for FSR / RCAS sharpness test)
  ctx.fillStyle = "#94a3b8";
  ctx.font = "bold 20px monospace";
  ctx.fillText("GLSS SUPERSAMPLING TEST PATTERN (SOURCE 24 FPS / 960x540)", 40, 50);

  ctx.font = "14px monospace";
  ctx.fillStyle = "#64748b";
  ctx.fillText(`FRAME COUNTER: ${frameIdx.toString().padStart(6, '0')} | TIME: ${(frameIdx / 24).toFixed(2)}s`, 40, 80);

  // Scrolling ticker banner for motion interpolation judder test
  const tickerX = (frameIdx * 4) % (canvas.width + 400) - 200;
  ctx.fillStyle = "rgba(99, 102, 241, 0.2)";
  ctx.fillRect(0, 460, canvas.width, 50);
  ctx.fillStyle = "#38bdf8";
  ctx.font = "bold 22px monospace";
  ctx.fillText(">>> 24 FPS TICKER TEXT - WATCH SMOOTHNESS AT 60/120 FPS INTERPOLATION >>>", tickerX, 492);

  // Moving bouncing balls for SAD motion estimation
  for (const b of balls) {
    b.x += b.vx;
    b.y += b.vy;
    if (b.x - b.r < 0 || b.x + b.r > canvas.width) b.vx *= -1;
    if (b.y - b.r < 100 || b.y + b.r > 450) b.vy *= -1;

    ctx.beginPath();
    ctx.arc(b.x, b.y, b.r, 0, Math.PI * 2);
    ctx.fillStyle = b.color;
    ctx.fill();
    ctx.lineWidth = 3;
    ctx.strokeStyle = "#ffffff";
    ctx.stroke();

    // Inner details for sharpness test
    ctx.fillStyle = "#ffffff";
    ctx.font = "bold 12px sans-serif";
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillText(`${Math.round(b.x)},${Math.round(b.y)}`, b.x, b.y);
  }
}

// 24 FPS timer
setInterval(drawDemoFrame, 1000 / 24);
