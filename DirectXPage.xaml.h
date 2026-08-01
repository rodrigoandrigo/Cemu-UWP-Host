#pragma once

#include "DirectXPage.g.h"
#include "Cemu_UWP_HostMain.h"
#include "Common/DeviceResources.h"
#include <chrono>

namespace Cemu_UWP_Host
{
	public ref class DirectXPage sealed
	{
	public:
		DirectXPage();
		virtual ~DirectXPage();
		void SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state);
		void LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state);

	private:
		void InitializeEmulator(float width, float height);
		void UpdateEmulatorSurfaceSize(float width, float height);
		void OnRendering(Platform::Object^ sender, Platform::Object^ args);
		void OnGamepadAdded(Platform::Object^ sender, Windows::Gaming::Input::Gamepad^ gamepad);
		void OnGamepadRemoved(Platform::Object^ sender, Windows::Gaming::Input::Gamepad^ gamepad);
		void OnRawGameControllerAdded(Platform::Object^ sender,
			Windows::Gaming::Input::RawGameController^ controller);
		void OnRawGameControllerRemoved(Platform::Object^ sender,
			Windows::Gaming::Input::RawGameController^ controller);
		void InstallContent_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void InstallGraphicPacks_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void RefreshLibrary_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void InstalledGames_SelectionChanged(Platform::Object^ sender,
			Windows::UI::Xaml::Controls::SelectionChangedEventArgs^ args);
		void StartGame_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ToggleTabs_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void ClearErrors_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void EmulatorViewport_PointerPressed(Platform::Object^ sender,
			Windows::UI::Xaml::Input::PointerRoutedEventArgs^ args);
		void EmulatorViewport_SizeChanged(Platform::Object^ sender, Windows::UI::Xaml::SizeChangedEventArgs^ args);
		void EmulatorSurface_CompositionScaleChanged(
			Windows::UI::Xaml::Controls::SwapChainPanel^ sender,
			Platform::Object^ args);
		void FocusEmulatorInput();
		void SetTabsVisible(bool visible);
		void AppendError(const std::string& message);
		void OnCemuStateChanged(CemuEmbedState state);
		void OnBrokeredProgress(uint64_t bytesCopied, uint64_t totalBytes, const std::string& relativePath);
		void BeginInstall();
		void RefreshLibrary();
		void SetLibraryActionsEnabled(bool enabled);
		void UpdateStartButton();
		void UpdateGamepadStatus();
		void UpdateActiveAccount();
		void TryConfigureDefaultGamepad();
		void UpdateVirtualMouse();
		void SetVirtualMouseEnabled(bool enabled);
		std::shared_ptr<DX::DeviceResources> m_deviceResources;
		std::unique_ptr<Cemu_UWP_HostMain> m_main;
		std::vector<InstalledTitle> m_installedTitles;
		Windows::Foundation::EventRegistrationToken m_renderingToken{};
		Windows::Foundation::EventRegistrationToken m_gamepadAddedToken{};
		Windows::Foundation::EventRegistrationToken m_gamepadRemovedToken{};
		Windows::Foundation::EventRegistrationToken m_rawControllerAddedToken{};
		Windows::Foundation::EventRegistrationToken m_rawControllerRemovedToken{};
		bool m_cemuReady = false;
		bool m_libraryBusy = false;
		bool m_gamepadProfileReady = false;
		bool m_gameRunning = false;
		bool m_virtualMouseEnabled = false;
		bool m_virtualMouseChordHeld = false;
		bool m_virtualMouseLeftDown = false;
		double m_virtualMouseX = 0.0;
		double m_virtualMouseY = 0.0;
		std::chrono::steady_clock::time_point m_virtualMouseLastUpdate{};
		unsigned int m_gamepadRetryFrames = 0;
		unsigned int m_controllerPollFrames = 0;
	};
}
