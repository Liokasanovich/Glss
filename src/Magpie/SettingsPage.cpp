#include "pch.h"
#include "SettingsPage.h"
#if __has_include("SettingsPage.g.cpp")
#include "SettingsPage.g.cpp"
#endif
#include "ControlHelper.h"
// Need App and RootPage declarations to call App::Get() and RootPage methods
#include "App.h"
#include "RootPage.h"

using namespace ::Magpie;
using namespace winrt;
using namespace Windows::UI::Xaml::Input;

namespace winrt::Magpie::implementation {

void SettingsPage::InitializeComponent() {
	SettingsPageT::InitializeComponent();

	ResourceLoader resourceLoader =
		ResourceLoader::GetForCurrentView(L"Microsoft.UI.Xaml/Resources");
	hstring versionStr = resourceLoader.GetString(L"SettingsButtonName");
	SettingsPageFrame().Title(versionStr);
}

void SettingsPage::ComboBox_DropDownOpened(IInspectable const& sender, IInspectable const&) const {
	ControlHelper::ComboBox_DropDownOpened(sender);
}

void SettingsPage::AboutLink_Click(IInspectable const&, RoutedEventArgs const&) {
	App::Get().RootPage()->NavigateToAboutPage();
}

void SettingsPage::MainLink_Click(IInspectable const&, RoutedEventArgs const&) {
	App::Get().RootPage()->NavigateToHomePage();
}

}
