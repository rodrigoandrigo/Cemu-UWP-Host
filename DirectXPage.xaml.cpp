#include "pch.h"
#include "DirectXPage.xaml.h"

#include <cmath>
#include <iomanip>
#include <sstream>

using namespace Cemu_UWP_Host;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;

namespace
{
const auto VisibleValue = static_cast<Windows::UI::Xaml::Visibility>(0);
const auto CollapsedValue = static_cast<Windows::UI::Xaml::Visibility>(1);

Platform::String^ FromUtf8(const std::string& value)
{
	if (value.empty()) return "";
	const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
	if (length <= 0) return "";
	std::wstring converted(length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &converted[0], length);
	return ref new Platform::String(converted.c_str(), static_cast<unsigned int>(converted.size()));
}

bool HasGamepadButton(GamepadButtons buttons, GamepadButtons button)
{
	return (static_cast<unsigned int>(buttons) & static_cast<unsigned int>(button)) != 0;
}

uint32_t NormalizeGamepadButtons(GamepadButtons buttons)
{
	uint32_t result = 0;
	auto set = [&result, buttons](GamepadButtons button, uint32_t bit)
	{
		if (HasGamepadButton(buttons, button)) result |= 1u << bit;
	};
	set(GamepadButtons::A, 0); set(GamepadButtons::B, 1);
	set(GamepadButtons::X, 2); set(GamepadButtons::Y, 3);
	set(GamepadButtons::View, 4); set(GamepadButtons::Menu, 6);
	set(GamepadButtons::LeftThumbstick, 7); set(GamepadButtons::RightThumbstick, 8);
	set(GamepadButtons::LeftShoulder, 9); set(GamepadButtons::RightShoulder, 10);
	set(GamepadButtons::DPadUp, 11); set(GamepadButtons::DPadDown, 12);
	set(GamepadButtons::DPadLeft, 13); set(GamepadButtons::DPadRight, 14);
	return result;
}

double ApplyStickDeadzone(double value)
{
	constexpr double deadzone = 0.18;
	const double magnitude = std::abs(value);
	if (magnitude <= deadzone)
		return 0.0;
	const double normalized = (magnitude - deadzone) / (1.0 - deadzone);
	return std::copysign(normalized, value);
}

bool IsGamepadVirtualKey(VirtualKey key)
{
	switch (key)
	{
	case VirtualKey::GamepadA:
	case VirtualKey::GamepadB:
	case VirtualKey::GamepadX:
	case VirtualKey::GamepadY:
	case VirtualKey::GamepadRightShoulder:
	case VirtualKey::GamepadLeftShoulder:
	case VirtualKey::GamepadLeftTrigger:
	case VirtualKey::GamepadRightTrigger:
	case VirtualKey::GamepadDPadUp:
	case VirtualKey::GamepadDPadDown:
	case VirtualKey::GamepadDPadLeft:
	case VirtualKey::GamepadDPadRight:
	case VirtualKey::GamepadMenu:
	case VirtualKey::GamepadView:
	case VirtualKey::GamepadLeftThumbstickButton:
	case VirtualKey::GamepadRightThumbstickButton:
	case VirtualKey::GamepadLeftThumbstickUp:
	case VirtualKey::GamepadLeftThumbstickDown:
	case VirtualKey::GamepadLeftThumbstickRight:
	case VirtualKey::GamepadLeftThumbstickLeft:
	case VirtualKey::GamepadRightThumbstickUp:
	case VirtualKey::GamepadRightThumbstickDown:
	case VirtualKey::GamepadRightThumbstickRight:
	case VirtualKey::GamepadRightThumbstickLeft:
		return true;
	default:
		return false;
	}
}

bool IsGamepadThumbstickNavigationKey(VirtualKey key)
{
	switch (key)
	{
	case VirtualKey::GamepadLeftThumbstickUp:
	case VirtualKey::GamepadLeftThumbstickDown:
	case VirtualKey::GamepadLeftThumbstickRight:
	case VirtualKey::GamepadLeftThumbstickLeft:
	case VirtualKey::GamepadRightThumbstickUp:
	case VirtualKey::GamepadRightThumbstickDown:
	case VirtualKey::GamepadRightThumbstickRight:
	case VirtualKey::GamepadRightThumbstickLeft:
		return true;
	default:
		return false;
	}
}
}

// This is the desktop interop interface implemented by CoreWindow. Do not use
// the 79B9... IID here: that is the WinRT ICoreWindow interface and has a
// completely different vtable.
struct __declspec(uuid("45D64A29-A63E-4CB6-B498-5781D298CB4F")) ICoreWindowInterop : IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE get_WindowHandle(HWND* value) = 0;
	virtual HRESULT STDMETHODCALLTYPE put_MessageHandled(boolean value) = 0;
};

DirectXPage::DirectXPage()
{
	InitializeComponent();
	// Registering WGI events on the XAML thread. The host mirrors a plain
	// controller snapshot into Cemu, so the DLL never has to use a WGI object
	// from SDL's worker apartment on Xbox.
	m_gamepadAddedToken =
		Gamepad::GamepadAdded += ref new EventHandler<Gamepad^>(this, &DirectXPage::OnGamepadAdded);
	m_gamepadRemovedToken =
		Gamepad::GamepadRemoved += ref new EventHandler<Gamepad^>(this, &DirectXPage::OnGamepadRemoved);
	// Xbox also projects controller buttons as CoreWindow keys. Consume that
	// parallel XAML navigation route while a title is running; otherwise B can
	// become a system Back request even though the same button was already sent
	// to Cemu through the apartment-owned WGI snapshot.
	auto coreWindow = Window::Current->CoreWindow;
	m_coreWindowKeyDownToken = coreWindow->KeyDown +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			this, &DirectXPage::OnCoreWindowKeyDown);
	m_coreWindowKeyUpToken = coreWindow->KeyUp +=
		ref new TypedEventHandler<CoreWindow^, KeyEventArgs^>(
			this, &DirectXPage::OnCoreWindowKeyDown);
	m_backRequestedToken = SystemNavigationManager::GetForCurrentView()->BackRequested +=
		ref new EventHandler<BackRequestedEventArgs^>(this, &DirectXPage::OnBackRequested);
	// The Added event is not guaranteed to be replayed for a controller that
	// was connected before the app started. Enumerate once, then rely only on
	// Added/Removed to maintain this apartment-owned reference.
	try
	{
		auto gamepads = Gamepad::Gamepads;
		if (gamepads && gamepads->Size != 0)
			m_gamepad = gamepads->GetAt(0);
	}
	catch (Platform::Exception^)
	{
		m_gamepad = nullptr;
	}
	m_renderingToken =
		CompositionTarget::Rendering += ref new EventHandler<Platform::Object^>(this, &DirectXPage::OnRendering);
	UpdateGamepadStatus();
}

void DirectXPage::InitializeEmulator(float width, float height)
{
	if (m_main || width <= 0.0f || height <= 0.0f)
		return;

	// Creating the template swap chain in the page constructor gives it a 1x1
	// back buffer because XAML has not performed layout yet. Hand it to Cemu
	// only after the first real SizeChanged event.
	// Cemu owns all D3D11 rendering once the swap chain is attached. Avoid
	// allocating the template's unused D2D/DWrite/WIC and depth resources.
	m_deviceResources = std::make_shared<DX::DeviceResources>(true);
	m_deviceResources->SetSwapChainPanel(emulatorSurface);
	// SetSwapChainPanel already captures ActualWidth/ActualHeight after the
	// SizeChanged event. Resizing the just-created composition swap chain a
	// second time here can race XAML diagnostics/composition and produces
	// spurious E_INVALIDARG/invalid-call diagnostics.
	const auto outputSize = m_deviceResources->GetOutputSize();

	auto window = Window::Current->CoreWindow;
	Microsoft::WRL::ComPtr<ICoreWindowInterop> windowInterop;
	if (SUCCEEDED(reinterpret_cast<IInspectable*>(window)->QueryInterface(
		__uuidof(ICoreWindowInterop), reinterpret_cast<void**>(windowInterop.GetAddressOf()))))
	{
		HWND hwnd = nullptr;
			if (SUCCEEDED(windowInterop->get_WindowHandle(&hwnd)) && hwnd)
			{
				const auto dpiScale = m_deviceResources->GetDpi() / 96.0;
				const CemuEmbedD3D11Surface d3d11Surface{
					sizeof(CemuEmbedD3D11Surface),
					CEMU_EMBED_D3D11_SURFACE_VERSION,
				m_deviceResources->GetD3DDevice(),
				m_deviceResources->GetD3DDeviceContext(),
					m_deviceResources->GetSwapChain(),
					nullptr
				};
				m_deviceResources->ReleaseSizeDependentResourcesForExternalRenderer();
				m_main = std::make_shared<Cemu_UWP_HostMain>(
					hwnd, d3d11Surface,
					static_cast<int>(outputSize.Width),
					static_cast<int>(outputSize.Height), dpiScale);
			m_main->SetDiagnosticCallback([this](const std::string& message) { AppendError(message); });
			m_main->SetStateCallback([this](CemuEmbedState state) { OnCemuStateChanged(state); });
			m_main->SetProgressCallback([this](uint64_t copied, uint64_t total, const std::string& path)
			{
				OnBrokeredProgress(copied, total, path);
			});
			if (!m_main->Start())
			{
				AppendError("Failed to initialize Cemu or the Direct3D 11 backend.");
				launchStatus->Text = "Failed to initialize the emulator";
				m_main.reset();
			}
		}
	}
}

DirectXPage::~DirectXPage()
{
	SetSystemPointerForUi(true);
	CompositionTarget::Rendering -= m_renderingToken;
	Gamepad::GamepadAdded -= m_gamepadAddedToken;
	Gamepad::GamepadRemoved -= m_gamepadRemovedToken;
	Window::Current->CoreWindow->KeyDown -= m_coreWindowKeyDownToken;
	Window::Current->CoreWindow->KeyUp -= m_coreWindowKeyUpToken;
	SystemNavigationManager::GetForCurrentView()->BackRequested -= m_backRequestedToken;
	m_gamepad = nullptr;
	if (m_main)
	{
		m_main->SetVirtualMouse(0, 0, false, false);
		CemuEmbedGamepadState disconnected{};
		disconnected.struct_size = sizeof(disconnected);
		disconnected.abi_version = CEMU_EMBED_GAMEPAD_VERSION;
		m_main->SetGamepadState(disconnected);
	}
	m_main.reset();
}

void DirectXPage::OnRendering(Platform::Object^, Platform::Object^)
{
	// Xbox can recreate its controller-driven system cursor while pointer mode
	// remains Auto. Reassert the hidden cursor for every presented UI frame so
	// it cannot reappear over a running title.
	if (m_gameRunning)
		SetSystemPointerForUi(false);
	if (m_main) m_main->Pump();
	const auto gamepadState = PublishGamepadState();
	UpdateVirtualMouse(gamepadState);
	if (++m_controllerPollFrames >= 60)
	{
		m_controllerPollFrames = 0;
		UpdateGamepadStatus();
	}
	// A controller profile changes Cemu's input topology.  On Xbox this must
	// complete before the title starts; changing it while Latte is consuming
	// controller state can terminate the packaged process.
	if (!m_gameRunning && m_cemuReady && !m_gamepadProfileReady && m_gamepad != nullptr &&
		++m_gamepadRetryFrames >= 60)
	{
		m_gamepadRetryFrames = 0;
		TryConfigureDefaultGamepad();
	}
}

void DirectXPage::OnGamepadAdded(Platform::Object^, Gamepad^ gamepad)
{
	// Capture the WGI state while it belongs to the XAML apartment. Cemu uses
	// the mirrored host state instead of opening this controller through SDL.
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this, gamepad]()
		{
			// WGI raises device events on a system callback thread.  Keep all
			// page state on the XAML dispatcher; racing OnRendering here can make
			// the Xbox terminate the packaged game without a managed exception.
			m_gamepad = gamepad;
			m_gamepadProfileReady = false;
			m_gamepadRetryFrames = 59;
			PublishGamepadState();
			UpdateGamepadStatus();
		})));
}

void DirectXPage::OnGamepadRemoved(Platform::Object^, Gamepad^ gamepad)
{
	// Publish a disconnected host state before updating the UI.
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this, gamepad]()
		{
			if (m_gamepad == gamepad)
				m_gamepad = nullptr;
			m_gamepadProfileReady = false;
			PublishGamepadState();
			if (m_virtualMouseEnabled)
				SetVirtualMouseEnabled(false);
			else
				UpdateGamepadStatus();
		})));
}

void DirectXPage::OnCoreWindowKeyDown(CoreWindow^, KeyEventArgs^ args)
{
	if (!args || !IsGamepadVirtualKey(args->VirtualKey))
		return;
	// Before launch only the D-pad controls directional UI focus. Analog-stick
	// virtual keys are consumed so Xbox cannot switch back to pointer-like
	// navigation. Once a title runs every gamepad key belongs exclusively to
	// the emulated Wii U controller.
	if (m_gameRunning || IsGamepadThumbstickNavigationKey(args->VirtualKey))
		args->Handled = true;
}

void DirectXPage::OnBackRequested(Platform::Object^, BackRequestedEventArgs^ args)
{
	if (m_gameRunning && args)
	{
		args->Handled = true;
		OutputDebugStringW(L"[Cemu/UWP host] Xbox Back navigation suppressed while a title is running.\n");
	}
}

void DirectXPage::InstallContent_Click(Platform::Object^, RoutedEventArgs^)
{
	BeginInstall();
}

void DirectXPage::InstallGraphicPacks_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy)
		return;
	auto picker = ref new FolderPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append("*");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFolderAsync()).then([weakThis](StorageFolder^ folder)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !folder) return;
		page->m_libraryBusy = true;
		page->SetLibraryActionsEnabled(false);
		page->startButton->IsEnabled = false;
		page->launchStatus->Text = "Importing graphic packs...";
		const uint64_t selectedTitleId = page->m_selectedTitleId;
		std::vector<uint64_t> titleIds;
		if (selectedTitleId)
			titleIds.push_back(selectedTitleId);
		else
			for (const auto& title : page->m_installedTitles)
				titleIds.push_back(title.titleId);
		const auto main = page->m_main;
		create_task([main, folder, titleIds]()
		{
			uint32_t imported{};
			if (!main || !main->InstallGraphicPacks(folder, &imported))
				return std::pair<uint32_t, uint32_t>{};
			uint32_t enabled{};
			for (const auto titleId : titleIds)
			{
				uint32_t affected{};
				if (main->ApplySafeGraphicPackPolicyForTitle(titleId, &affected))
					enabled += affected;
			}
			return std::make_pair(imported, enabled);
		}).then([weakThis](std::pair<uint32_t, uint32_t> result)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->m_libraryBusy = false;
			page->SetLibraryActionsEnabled(true);
			if (!result.first)
			{
				page->launchStatus->Text = "Failed to import graphic packs; see Help and errors";
				page->SetTabsVisible(true);
				page->toolTabs->SelectedIndex = 1;
				page->UpdateStartButton();
				return;
			}
			std::ostringstream status;
			status << result.first << " graphic pack(s) imported";
			if (result.second)
				status << "; " << result.second << " adjusted by the safe policy";
			page->launchStatus->Text = FromUtf8(status.str());
			page->RefreshLibrary();
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::RefreshLibrary_Click(Platform::Object^, RoutedEventArgs^)
{
	RefreshLibrary();
}

void DirectXPage::InstalledGames_SelectionChanged(Platform::Object^,
	SelectionChangedEventArgs^)
{
	const int selectedIndex = installedGamesList->SelectedIndex;
	const int committedIndex = FindInstalledTitleIndex(m_selectedTitleId);
	if (m_libraryBusy)
	{
		UpdateStartButton();
		return;
	}

	// SelectionChanged is also raised while Xbox moves focus with the D-pad.
	// That is navigation, not confirmation. Only ItemClick (Gamepad A or a
	// pointer click) is allowed to replace m_selectedTitleId.
	if (committedIndex >= 0 && selectedIndex != committedIndex)
	{
		// Xbox's controller-driven pointer can move ListView focus and cause a
		// transient selection change. Restore the title confirmed with A in the
		// same event so the cursor never becomes the authoritative selection.
		if (!m_restoringCommittedSelection)
		{
			m_restoringCommittedSelection = true;
			installedGamesList->SelectedIndex = committedIndex;
			m_restoringCommittedSelection = false;
		}
		launchStatus->Text = "Ready to start";
		UpdateStartButton();
		return;
	}
	else if (committedIndex >= 0)
	{
		launchStatus->Text = "Ready to start";
	}
	UpdateStartButton();
}

void DirectXPage::InstalledGames_ItemClick(Platform::Object^,
	ItemClickEventArgs^ args)
{
	if (!args || !args->ClickedItem || m_libraryBusy)
		return;

	// Xbox pointer/controller activation is not guaranteed to leave the
	// ListViewItem focused after the routed PointerPressed reaches the emulator
	// viewport. Commit the clicked item explicitly instead of depending on the
	// transient focus visual.
	for (unsigned int index = 0; index < installedGamesList->Items->Size; ++index)
	{
		if (installedGamesList->Items->GetAt(index) != args->ClickedItem)
			continue;
		if (index < m_installedTitles.size())
			m_selectedTitleId = m_installedTitles[index].titleId;
		// Commit before assigning SelectedIndex. SelectionChanged can run
		// synchronously and must already see the new title as authoritative.
		installedGamesList->SelectedIndex = static_cast<int>(index);
		launchStatus->Text = "Ready to start";
		UpdateStartButton();
		break;
	}
}

void DirectXPage::StartGame_Click(Platform::Object^, RoutedEventArgs^)
{
	const int selectedIndex = FindInstalledTitleIndex(m_selectedTitleId);
	if (!m_main || !m_main->IsReady() || selectedIndex < 0)
		return;
	const uint64_t titleId = m_installedTitles[selectedIndex].titleId;
	// Keep Windows.Gaming.Input on the XAML apartment and finish the plain
	// Cemu profile setup before the game creates its input threads.  This is
	// the Xbox/Durango-safe lifetime model: no WGI object crosses into Cemu.
	const auto gamepadState = PublishGamepadState();
	if (gamepadState.connected)
	{
		TryConfigureDefaultGamepad();
		if (!m_gamepadProfileReady)
		{
			launchStatus->Text = "Could not prepare the Xbox Controller profile";
			AppendError("The Wii U GamePad profile was not ready before starting the game.");
			UpdateGamepadStatus();
			return;
		}
	}
	SetTabsVisible(false);
	startButton->IsEnabled = false;
	SetLibraryActionsEnabled(false);
	// The controller-driven system cursor is useful for the XAML library but
	// must disappear before game input is handed exclusively to the emulator.
	SetSystemPointerForUi(false);
	// Begin every title with the optional pointer disabled. Games that need a
	// Wii U GamePad pointer can enable it with L+R; A remains its left click.
	m_main->SetVirtualMouse(0, 0, false, false);
	// LaunchInstalledTitle() starts Cemu title threads before its task
	// continuation runs, so block profile replacement from this point onward.
	m_gameRunning = true;
	launchStatus->Text = "Mounting game, update, and DLC...";
	emulatorPlaceholder->Visibility = CollapsedValue;
	FocusEmulatorInput();
	create_task([this, titleId]()
	{
		if (!m_main) return false;
		uint32_t enabledPacks{};
		m_main->ApplySafeGraphicPackPolicyForTitle(titleId, &enabledPacks);
		return m_main->LaunchInstalledTitle(titleId);
	}).then([this](bool launched)
	{
		if (launched)
		{
			m_gameRunning = true;
			UpdateGamepadStatus();
			launchStatus->Text = "Game running";
			FocusEmulatorInput();
		}
		else
		{
			m_gameRunning = false;
			SetSystemPointerForUi(true);
			launchStatus->Text = "Could not start the installed game";
			AppendError("Could not mount the base game with the installed update and DLC.");
			SetLibraryActionsEnabled(true);
			UpdateStartButton();
			emulatorPlaceholder->Visibility = VisibleValue;
		}
	}, task_continuation_context::use_current());
}

void DirectXPage::ToggleMetrics_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady)
		return;

	const bool show = !m_performanceMetricsVisible;
	if (!m_main->SetPerformanceMetrics(show))
	{
		AppendError("Could not change the Cemu performance metrics overlay.");
		return;
	}

	m_performanceMetricsVisible = show;
	metricsButtonText->Text = show ? "Hide metrics" : "Show metrics";
}

void DirectXPage::BeginInstall()
{
	if (!m_main || !m_cemuReady || m_libraryBusy)
		return;
	auto picker = ref new FolderPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append("*");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFolderAsync()).then(
		[weakThis](StorageFolder^ folder)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !folder) return;
		page->m_libraryBusy = true;
		page->SetLibraryActionsEnabled(false);
		page->startButton->IsEnabled = false;
		page->launchStatus->Text = "Installing content...";
		const auto main = page->m_main;
		create_task([main, folder]()
		{
			uint64_t baseTitleId{};
			const bool installed = main &&
				main->InstallTitle(folder, CEMU_EMBED_INSTALL_AUTO, &baseTitleId);
			return installed ? baseTitleId : uint64_t{};
		}).then([weakThis](uint64_t installedBaseTitleId)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			page->m_libraryBusy = false;
			page->SetLibraryActionsEnabled(true);
			if (!installedBaseTitleId)
			{
				page->launchStatus->Text = "Installation failed";
				page->UpdateStartButton();
				return;
			}
			page->launchStatus->Text = "Installation complete";
			page->RefreshLibrary();
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::RefreshLibrary()
{
	if (!m_main || !m_cemuReady || m_libraryBusy)
		return;
	m_libraryBusy = true;
	SetLibraryActionsEnabled(false);
	startButton->IsEnabled = false;
	launchStatus->Text = "Refreshing library...";
	create_task([this]()
	{
		return m_main ? m_main->GetInstalledTitles() : std::vector<InstalledTitle>{};
	}).then([this](std::vector<InstalledTitle> titles)
	{
		m_installedTitles = std::move(titles);
		installedGamesList->Items->Clear();
		for (const auto& title : m_installedTitles)
		{
			std::ostringstream line;
			line << title.name << "\nTitle ID: "
				<< std::uppercase << std::hex << std::setw(16)
				<< std::setfill('0') << title.titleId << std::dec
				<< "  |  Version: v" << title.effectiveVersion
				<< " (base v" << title.baseVersion;
			if (title.updateVersion)
				line << ", update v" << title.updateVersion;
			else
				line << ", no update";
			line << ")  |  DLC: ";
			if (title.dlcCount)
				line << title.dlcCount << " installed, v" << title.dlcVersion;
			else
				line << "not installed";
			line << "  |  Region: " << title.regionName
				<< "  |  Graphic packs: " << title.enabledGraphicPackCount
				<< "/" << title.compatibleGraphicPackCount << " active";
			installedGamesList->Items->Append(FromUtf8(line.str()));
		}
		const int restoredIndex = FindInstalledTitleIndex(m_selectedTitleId);
		installedGamesList->SelectedIndex = restoredIndex;
		if (restoredIndex < 0)
			m_selectedTitleId = 0;
		m_libraryBusy = false;
		SetLibraryActionsEnabled(true);
		launchStatus->Text = m_installedTitles.empty()
			? "No games installed"
			: (restoredIndex >= 0 ? "Ready to start" : "Select an installed game");
		UpdateStartButton();
	}, task_continuation_context::use_current());
}

void DirectXPage::SetLibraryActionsEnabled(bool enabled)
{
	const bool canUse = enabled && m_cemuReady;
	installContentButton->IsEnabled = canUse;
	refreshLibraryButton->IsEnabled = canUse;
	installGraphicPacksButton->IsEnabled = canUse;
	installedGamesList->IsEnabled = enabled;
}

void DirectXPage::ToggleTabs_Click(Platform::Object^, RoutedEventArgs^)
{
	SetTabsVisible(tabsPanel->Visibility != VisibleValue);
}

void DirectXPage::ClearErrors_Click(Platform::Object^, RoutedEventArgs^)
{
	errorsList->Items->Clear();
}

void DirectXPage::EmulatorViewport_PointerPressed(
	Platform::Object^, Windows::UI::Xaml::Input::PointerRoutedEventArgs^)
{
	if (!m_gameRunning)
		return;
	FocusEmulatorInput();
}

void DirectXPage::FocusEmulatorInput()
{
	// SwapChainPanel is not a Control and cannot own XAML focus. The Page is a
	// focusable Control, so focusing it releases ListView/AppBarButton focus
	// while CoreWindow, SDL3-UWP and Windows.Gaming.Input continue receiving
	// keyboard and controller input for Cemu.
	toggleTabsButton->IsTabStop = false;
	startButton->IsTabStop = false;
	metricsButton->IsTabStop = false;
	this->Focus(Windows::UI::Xaml::FocusState::Programmatic);
}

void DirectXPage::SetSystemPointerForUi(bool enabled)
{
	// Application.RequiresPointerMode can only be established during app
	// startup on this UWP runtime. CoreWindow.PointerCursor is the supported
	// runtime switch for hiding and restoring the controller-driven cursor.
	if (enabled && !m_systemPointerHidden)
		return;
	try
	{
		auto window = Window::Current;
		if (!window || !window->CoreWindow)
			return;
		auto coreWindow = window->CoreWindow;
		if (enabled)
		{
			coreWindow->PointerCursor = m_savedSystemPointerCursor;
			m_savedSystemPointerCursor = nullptr;
			m_systemPointerHidden = false;
		}
		else
		{
			// Save the UI cursor only on the first transition. If Xbox recreates a
			// cursor later, discard it instead of replacing the cursor to restore.
			if (!m_systemPointerHidden)
				m_savedSystemPointerCursor = coreWindow->PointerCursor;
			if (coreWindow->PointerCursor != nullptr)
				coreWindow->PointerCursor = nullptr;
			m_systemPointerHidden = true;
		}
	}
	catch (Platform::Exception^)
	{
		// Cursor visibility must never interrupt launch or shutdown.
	}
}

void DirectXPage::EmulatorViewport_SizeChanged(Platform::Object^, SizeChangedEventArgs^ args)
{
	if (args->NewSize.Width <= 0 || args->NewSize.Height <= 0) return;
	if (!m_main)
	{
		InitializeEmulator(args->NewSize.Width, args->NewSize.Height);
		return;
	}
	UpdateEmulatorSurfaceSize(args->NewSize.Width, args->NewSize.Height);
}

void DirectXPage::EmulatorSurface_CompositionScaleChanged(
	SwapChainPanel^ sender, Platform::Object^)
{
	if (!sender)
		return;
	UpdateEmulatorSurfaceSize(
		static_cast<float>(sender->ActualWidth),
		static_cast<float>(sender->ActualHeight));
}

void DirectXPage::UpdateEmulatorSurfaceSize(float width, float height)
{
	if (!m_main || width <= 0.0f || height <= 0.0f)
		return;
	// ResizeBuffers is performed by Cemu's D3D11 presentation thread, where all
	// back-buffer references can be released safely. The XAML thread only
	// publishes the new composition-pixel size.
	double scaleX = emulatorSurface->CompositionScaleX;
	double scaleY = emulatorSurface->CompositionScaleY;
	if (scaleX <= 0.0 || scaleY <= 0.0)
	{
		const auto dpiScale =
			Windows::Graphics::Display::DisplayInformation::GetForCurrentView()->LogicalDpi / 96.0;
		scaleX = scaleY = dpiScale;
	}
	m_main->ResizeSurface(
		static_cast<int>(std::lround(width * scaleX)),
		static_cast<int>(std::lround(height * scaleY)),
		(scaleX + scaleY) * 0.5);
}

void DirectXPage::SetTabsVisible(bool visible)
{
	tabsPanel->Visibility = visible ? VisibleValue : CollapsedValue;
	toggleTabsButtonText->Text = visible ? "Hide options" : "Show options";
	if (!visible && m_gameRunning)
		FocusEmulatorInput();
}

void DirectXPage::AppendError(const std::string& message)
{
	auto text = FromUtf8(message);
	if (Dispatcher->HasThreadAccess)
	{
		errorsList->Items->Append(text);
		return;
	}
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this, text]()
		{
			errorsList->Items->Append(text);
		})));
}

void DirectXPage::OnCemuStateChanged(CemuEmbedState state)
{
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this, state]()
		{
			m_cemuReady = state == CEMU_EMBED_STATE_READY;
			metricsButton->IsEnabled = m_cemuReady;
			if (state == CEMU_EMBED_STATE_READY)
			{
				launchStatus->Text = "Loading library...";
				SetLibraryActionsEnabled(true);
				m_gamepadRetryFrames = 59;
				UpdateActiveAccount();
				TryConfigureDefaultGamepad();
				RefreshLibrary();
			}
			else if (state == CEMU_EMBED_STATE_INITIALIZING)
				launchStatus->Text = "Initializing emulator...";
			else if (state == CEMU_EMBED_STATE_FAILED)
			{
				launchStatus->Text = "Failed to initialize the emulator";
				accountStatus->Text = "Account unavailable";
				accountStatusIcon->Opacity = 0.45;
			}
			if (state != CEMU_EMBED_STATE_READY)
			{
				m_gameRunning = false;
				m_performanceMetricsVisible = false;
				metricsButtonText->Text = "Show metrics";
				SetSystemPointerForUi(true);
				if (m_virtualMouseEnabled)
					SetVirtualMouseEnabled(false);
				SetLibraryActionsEnabled(false);
			}
			UpdateStartButton();
		})));
}

void DirectXPage::SetVirtualMouseEnabled(bool enabled)
{
	if (m_virtualMouseEnabled == enabled)
		return;

	m_virtualMouseEnabled = enabled;
	m_virtualMouseLeftDown = false;
	m_virtualMouseLastUpdate = std::chrono::steady_clock::now();

	if (enabled)
	{
		const double width = emulatorViewport->ActualWidth;
		const double height = emulatorViewport->ActualHeight;
		if (m_virtualMouseX <= 0.0 || m_virtualMouseY <= 0.0)
		{
			m_virtualMouseX = width * 0.5;
			m_virtualMouseY = height * 0.5;
		}
		m_virtualMouseX = (std::max)(0.0, (std::min)(m_virtualMouseX, width));
		m_virtualMouseY = (std::max)(0.0, (std::min)(m_virtualMouseY, height));
		virtualMouseTransform->X = m_virtualMouseX;
		virtualMouseTransform->Y = m_virtualMouseY;
		virtualMouseCursor->Visibility = VisibleValue;

		const double scaleX = emulatorSurface->CompositionScaleX > 0.0
			? emulatorSurface->CompositionScaleX : 1.0;
		const double scaleY = emulatorSurface->CompositionScaleY > 0.0
			? emulatorSurface->CompositionScaleY : 1.0;
		if (m_main)
			m_main->SetVirtualMouse(
				static_cast<int>(std::lround(m_virtualMouseX * scaleX)),
				static_cast<int>(std::lround(m_virtualMouseY * scaleY)), false, true);
	}
	else
	{
		if (m_main)
			m_main->SetVirtualMouse(0, 0, false, false);
		virtualMouseCursor->Visibility = CollapsedValue;
	}

	UpdateGamepadStatus();
}

void DirectXPage::UpdateVirtualMouse(const CemuEmbedGamepadState& gamepad)
{
	if (!m_gameRunning || !m_gamepadProfileReady || !gamepad.connected)
	{
		m_virtualMouseChordHeld = false;
		if (m_virtualMouseEnabled)
			SetVirtualMouseEnabled(false);
		return;
	}

	constexpr uint32_t leftShoulder = 1u << 9;
	constexpr uint32_t rightShoulder = 1u << 10;
	constexpr uint32_t buttonA = 1u << 0;
	const bool chordPressed =
		(gamepad.buttons & leftShoulder) != 0 &&
		(gamepad.buttons & rightShoulder) != 0;
	if (chordPressed && !m_virtualMouseChordHeld)
		SetVirtualMouseEnabled(!m_virtualMouseEnabled);
	m_virtualMouseChordHeld = chordPressed;

	const auto now = std::chrono::steady_clock::now();
	if (!m_virtualMouseEnabled)
	{
		m_virtualMouseLastUpdate = now;
		return;
	}

	double elapsed = std::chrono::duration<double>(now - m_virtualMouseLastUpdate).count();
	m_virtualMouseLastUpdate = now;
	elapsed = (std::max)(0.0, (std::min)(elapsed, 0.05));
	constexpr double cursorSpeed = 900.0;
	m_virtualMouseX += ApplyStickDeadzone(gamepad.left_x) * cursorSpeed * elapsed;
	m_virtualMouseY -= ApplyStickDeadzone(gamepad.left_y) * cursorSpeed * elapsed;

	const double width = emulatorViewport->ActualWidth;
	const double height = emulatorViewport->ActualHeight;
	m_virtualMouseX = (std::max)(0.0, (std::min)(m_virtualMouseX, width));
	m_virtualMouseY = (std::max)(0.0, (std::min)(m_virtualMouseY, height));
	virtualMouseTransform->X = m_virtualMouseX;
	virtualMouseTransform->Y = m_virtualMouseY;
	m_virtualMouseLeftDown = (gamepad.buttons & buttonA) != 0;

	const double scaleX = emulatorSurface->CompositionScaleX > 0.0
		? emulatorSurface->CompositionScaleX : 1.0;
	const double scaleY = emulatorSurface->CompositionScaleY > 0.0
		? emulatorSurface->CompositionScaleY : 1.0;
	if (m_main)
		m_main->SetVirtualMouse(
			static_cast<int>(std::lround(m_virtualMouseX * scaleX)),
			static_cast<int>(std::lround(m_virtualMouseY * scaleY)),
			m_virtualMouseLeftDown, true);
}

void DirectXPage::UpdateGamepadStatus()
{
	if (!m_gamepad)
	{
		controllerStatus->Text = "Controller disconnected";
		controllerStatus->Opacity = 0.65;
		controllerStatusIcon->Opacity = 0.45;
		m_gamepadProfileReady = false;
		return;
	}
	std::wostringstream text;
	text << L"Xbox Controller connected";
	if (m_gamepadProfileReady)
	{
		text << L" \u2022 Wii U GamePad profile";
		if (m_virtualMouseEnabled)
			text << L" \u2022 virtual mouse active (A: click)";
		else if (m_gameRunning)
			text << L" \u2022 L+R: virtual mouse";
	}
	else
		text << L" \u2022 preparing profile";
	controllerStatus->Text = ref new Platform::String(text.str().c_str());
	controllerStatus->Opacity = 1.0;
	controllerStatusIcon->Opacity = 1.0;
}

CemuEmbedGamepadState DirectXPage::PublishGamepadState()
{
	CemuEmbedGamepadState state{};
	state.struct_size = sizeof(state);
	state.abi_version = CEMU_EMBED_GAMEPAD_VERSION;
	if (!m_main)
		return state;
	auto gamepad = m_gamepad;
	if (!gamepad)
	{
		if (!m_hasPublishedGamepadState ||
			std::memcmp(&state, &m_lastPublishedGamepadState, sizeof(state)) != 0)
		{
			m_main->SetGamepadState(state);
			m_lastPublishedGamepadState = state;
			m_hasPublishedGamepadState = true;
		}
		return state;
	}

	try
	{
		auto reading = gamepad->GetCurrentReading();
		state.connected = 1;
		state.buttons = NormalizeGamepadButtons(reading.Buttons);
		state.left_x = static_cast<float>(reading.LeftThumbstickX);
		state.left_y = static_cast<float>(reading.LeftThumbstickY);
		state.right_x = static_cast<float>(reading.RightThumbstickX);
		state.right_y = static_cast<float>(reading.RightThumbstickY);
		state.left_trigger = static_cast<float>(reading.LeftTrigger);
		state.right_trigger = static_cast<float>(reading.RightTrigger);
	}
	catch (Platform::Exception^)
	{
		// The Xbox may revoke the user-device association while a removal event
		// is in flight. Do not allow that WinRT exception to escape OnRendering.
		m_gamepad = nullptr;
		m_gamepadProfileReady = false;
	}
	if (!m_hasPublishedGamepadState ||
		std::memcmp(&state, &m_lastPublishedGamepadState, sizeof(state)) != 0)
	{
		m_main->SetGamepadState(state);
		m_lastPublishedGamepadState = state;
		m_hasPublishedGamepadState = true;
	}
	return state;
}

void DirectXPage::UpdateActiveAccount()
{
	ActiveAccount account;
	if (!m_main || !m_main->GetActiveAccount(account))
	{
		accountStatus->Text = "Account unavailable";
		accountStatus->Opacity = 0.65;
		accountStatusIcon->Opacity = 0.45;
		return;
	}

	std::ostringstream text;
	text << "Account: " << (account.miiName.empty() ? "default" : account.miiName)
		<< " (" << std::uppercase << std::hex << std::setw(8)
		<< std::setfill('0') << account.persistentId << ")";
	if (account.onlineEnabled)
		text << " \xE2\x80\xA2 online";
	accountStatus->Text = FromUtf8(text.str());
	accountStatus->Opacity = 1.0;
	accountStatusIcon->Opacity = 1.0;
}

void DirectXPage::TryConfigureDefaultGamepad()
{
	if (!m_main || !m_cemuReady || m_gameRunning || !m_gamepad)
		return;

	// Do not schedule this through a PPL worker.  The Xbox shell can deliver
	// Gamepad events while that worker races the Cemu input/update threads.
	// This call only consumes the already-published POD state and is made on
	// the XAML thread, before a title is allowed to run.
	const auto state = PublishGamepadState();
	if (!state.connected)
	{
		m_gamepadProfileReady = false;
		UpdateGamepadStatus();
		return;
	}
	m_gamepadProfileReady = m_main->EnsureDefaultGamepadProfile();
	UpdateGamepadStatus();
}

void DirectXPage::OnBrokeredProgress(uint64_t bytesCopied, uint64_t totalBytes, const std::string&)
{
	std::ostringstream status;
	const double copiedGiB = static_cast<double>(bytesCopied) / (1024.0 * 1024.0 * 1024.0);
	const double totalGiB = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);
	const double percent = totalBytes ? (100.0 * static_cast<double>(bytesCopied) / static_cast<double>(totalBytes)) : 0.0;
	status << "Copying title: " << std::fixed << std::setprecision(1) << percent
		<< "% (" << std::setprecision(2) << copiedGiB << " / " << totalGiB << " GiB)";
	auto text = FromUtf8(status.str());
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this, text]()
		{
			launchStatus->Text = text;
		})));
}

void DirectXPage::UpdateStartButton()
{
	startButton->IsEnabled = m_main != nullptr && m_cemuReady && !m_libraryBusy &&
		FindInstalledTitleIndex(m_selectedTitleId) >= 0;
}

int DirectXPage::FindInstalledTitleIndex(uint64_t titleId) const
{
	if (titleId == 0)
		return -1;
	for (size_t index = 0; index < m_installedTitles.size(); ++index)
		if (m_installedTitles[index].titleId == titleId)
			return static_cast<int>(index);
	return -1;
}

void DirectXPage::SaveInternalState(Windows::Foundation::Collections::IPropertySet^)
{
}

void DirectXPage::LoadInternalState(Windows::Foundation::Collections::IPropertySet^)
{
}
