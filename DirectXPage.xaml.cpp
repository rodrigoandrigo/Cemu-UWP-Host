#include "pch.h"
#include "DirectXPage.xaml.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

using namespace Cemu_UWP_Host;
using namespace concurrency;
using namespace Windows::Foundation;
using namespace Windows::Gaming::Input;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::System;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Media;

namespace
{
const auto VisibleValue = static_cast<Windows::UI::Xaml::Visibility>(0);
const auto CollapsedValue = static_cast<Windows::UI::Xaml::Visibility>(1);
constexpr wchar_t LocalInstallFolderName[] = L"GamesToInstall";
constexpr wchar_t LocalInstallMarkerName[] = L"cemu-installed.txt";
constexpr wchar_t LocalGraphicPackMarkerName[] = L"cemu-graphic-pack-installed.txt";

Platform::String^ WinRtString(const wchar_t* value)
{
	return ref new Platform::String(value);
}

std::string ToUtf8(Platform::String^ value)
{
	if (!value || value->IsEmpty())
		return {};
	const int length = WideCharToMultiByte(CP_UTF8, 0, value->Data(),
		static_cast<int>(value->Length()), nullptr, 0, nullptr, nullptr);
	if (length <= 0)
		return {};
	std::string converted(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value->Data(),
		static_cast<int>(value->Length()), &converted[0], length, nullptr, nullptr);
	return converted;
}

struct LocalGameFile
{
	uint64_t id{};
	std::string name;
	std::string path;
	std::string format;
};

struct LocalInstallScanResult
{
	uint32_t discovered{};
	uint32_t installed{};
	uint32_t failed{};
	uint32_t alreadyProcessed{};
	uint32_t markerWarnings{};
	uint32_t graphicPacksDiscovered{};
	uint32_t graphicPacksImported{};
	uint32_t graphicPackFailures{};
	uint32_t graphicPacksAlreadyProcessed{};
	uint32_t enabledGraphicPacks{};
	std::vector<LocalGameFile> localGameFiles;
};

bool IsSupportedLocalGameExtension(std::string extension)
{
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	return extension == ".wud" || extension == ".wux" ||
		extension == ".iso" || extension == ".wua" ||
		extension == ".wuhb" || extension == ".rpx" ||
		extension == ".elf";
}

uint64_t StableLocalGameId(std::string path)
{
	std::transform(path.begin(), path.end(), path.begin(),
		[](unsigned char value) { return static_cast<char>(std::tolower(value)); });
	uint64_t hash = 1469598103934665603ull;
	for (const unsigned char value : path)
	{
		hash ^= value;
		hash *= 1099511628211ull;
	}
	// Real Wii U title IDs occupy the low title namespace. Reserve the high
	// nibble for stable host-only list IDs so selection remains unambiguous.
	return (hash & 0x0FFFFFFFFFFFFFFFull) | 0xF000000000000000ull;
}

StorageFolder^ GetOrCreateLocalInstallFolder()
{
	auto root = create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync(
		WinRtString(LocalInstallFolderName),
		CreationCollisionOption::OpenIfExists)).get();
	if (!root)
		return nullptr;

	// Keep usage instructions beside the drop folder so they are also visible
	// to users copying content through Xbox Device Portal or an FTP client.
	auto readmeItem = create_task(root->TryGetItemAsync(
		WinRtString(L"README.txt"))).get();
	if (!readmeItem)
	{
		auto readme = create_task(root->CreateFileAsync(
			WinRtString(L"README.txt"),
			CreationCollisionOption::OpenIfExists)).get();
		create_task(FileIO::WriteTextAsync(readme,
			WinRtString(L"Cemu local installation folder\r\n\r\n"
			L"Copy each extracted Wii U base game, update, or DLC into its own subfolder.\r\n"
			L"Every title folder must contain code, content, and meta.\r\n"
			L"Graphic Pack folders are detected by their rules.txt file.\r\n"
			L"In the app, select Scan local folder to detect and install the content.\r\n"
			L"Successful sources receive a cemu-installed marker. Delete that marker to reinstall them.\r\n"))).get();
	}
	return root;
}

bool IsExtractedInstallTitle(StorageFolder^ folder)
{
	if (!folder)
		return false;
	auto code = dynamic_cast<StorageFolder^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"code"))).get());
	auto content = dynamic_cast<StorageFolder^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"content"))).get());
	auto meta = dynamic_cast<StorageFolder^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"meta"))).get());
	if (!code || !content || !meta)
		return false;
	return dynamic_cast<StorageFile^>(
		create_task(code->TryGetItemAsync(WinRtString(L"app.xml"))).get()) != nullptr &&
		dynamic_cast<StorageFile^>(
			create_task(meta->TryGetItemAsync(WinRtString(L"meta.xml"))).get()) != nullptr;
}

bool IsGraphicPackCollection(StorageFolder^ folder)
{
	if (!folder || !folder->Name)
		return false;
	const auto name = folder->Name->Data();
	return _wcsicmp(name, L"Graphic Packs") == 0 ||
		_wcsicmp(name, L"graphicPacks") == 0 ||
		_wcsicmp(name, L"downloadedGraphicPacks") == 0;
}

void FindLocalInstallContent(StorageFolder^ folder,
	std::vector<StorageFolder^>& titles, uint32_t& alreadyProcessed,
	std::vector<StorageFolder^>& graphicPacks,
	uint32_t& graphicPacksAlreadyProcessed,
	std::vector<LocalGameFile>& localGameFiles)
{
	if (!folder)
		return;
	if (IsExtractedInstallTitle(folder))
	{
		if (create_task(folder->TryGetItemAsync(
			WinRtString(LocalInstallMarkerName))).get())
			++alreadyProcessed;
		else
			titles.emplace_back(folder);
		// code/content/meta can contain many directories. Once a title root is
		// recognized, do not recurse into its payload.
		return;
	}
	if (IsGraphicPackCollection(folder))
	{
		if (create_task(folder->TryGetItemAsync(
			WinRtString(LocalGraphicPackMarkerName))).get())
			++graphicPacksAlreadyProcessed;
		else
			graphicPacks.emplace_back(folder);
		// Import a community collection in one broker operation instead of one
		// staging pass per rules.txt directory.
		return;
	}
	if (dynamic_cast<StorageFile^>(
		create_task(folder->TryGetItemAsync(WinRtString(L"rules.txt"))).get()))
	{
		if (create_task(folder->TryGetItemAsync(
			WinRtString(LocalGraphicPackMarkerName))).get())
			++graphicPacksAlreadyProcessed;
		else
			graphicPacks.emplace_back(folder);
		// A rules.txt directory is one complete Cemu Graphic Pack. Its shader,
		// patch, and preset subdirectories belong to that pack.
		return;
	}

	const auto files = create_task(folder->GetFilesAsync()).get();
	for (const auto& file : files)
	{
		auto extension = ToUtf8(file->FileType);
		if (!IsSupportedLocalGameExtension(extension))
			continue;
		std::transform(extension.begin(), extension.end(), extension.begin(),
			[](unsigned char value) { return static_cast<char>(std::toupper(value)); });
		const auto path = ToUtf8(file->Path);
		localGameFiles.push_back({ StableLocalGameId(path), ToUtf8(file->Name),
			path, extension });
	}

	const auto children = create_task(folder->GetFoldersAsync()).get();
	for (const auto& child : children)
		FindLocalInstallContent(child, titles, alreadyProcessed, graphicPacks,
			graphicPacksAlreadyProcessed, localGameFiles);
}

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
	// Always open the library in its compact layout. The guide remains available
	// through its toggle, but an older persisted expanded state is not restored.
	SetGettingStartedExpanded(false);
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
	constexpr uint32_t viewButton = 1u << 4;
	constexpr uint32_t menuButton = 1u << 6;
	const bool optionsChord = m_gameRunning &&
		(gamepadState.buttons & viewButton) != 0 &&
		(gamepadState.buttons & menuButton) != 0;
	if (optionsChord && !m_optionsChordHeld)
	{
		const bool showOptions = tabsPanel->Visibility != VisibleValue;
		if (showOptions)
			toolTabs->SelectedIndex = 1;
		SetTabsVisible(showOptions);
	}
	m_optionsChordHeld = optionsChord;
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
	// While the options panel is open, projected D-pad/A keys belong to XAML and
	// the WGI snapshot sent to Cemu is neutral. With the panel hidden, every
	// gamepad key belongs exclusively to the emulated Wii U controller.
	const bool optionsVisible = tabsPanel->Visibility == VisibleValue;
	if ((m_gameRunning && !optionsVisible) || IsGamepadThumbstickNavigationKey(args->VirtualKey))
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

void DirectXPage::OpenGameFile_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	auto picker = ref new FileOpenPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	for (const auto extension : { L".wud", L".wux", L".iso", L".wua",
		L".wuhb", L".rpx", L".elf" })
		picker->FileTypeFilter->Append(ref new Platform::String(extension));
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFileAsync()).then([weakThis](StorageFile^ file)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !file) return;
		page->m_libraryBusy = true;
		page->SetLibraryActionsEnabled(false);
		page->startButton->IsEnabled = false;
		page->launchStatus->Text = "Copying selected game file...";
		auto cache = ApplicationData::Current->LocalCacheFolder;
		create_task(cache->CreateFolderAsync("brokered-game-files",
			CreationCollisionOption::OpenIfExists)).then([file](StorageFolder^ folder)
		{
			return create_task(file->CopyAsync(folder, file->Name,
				NameCollisionOption::ReplaceExisting));
		}).then([weakThis](task<StorageFile^> copyTask)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			try
			{
				auto copiedFile = copyTask.get();
				page->m_libraryBusy = false;
				const auto main = page->m_main;
				page->BeginExternalLaunch([main, copiedFile]()
				{
					return main && main->LaunchGameFile(copiedFile);
				});
			}
			catch (Platform::Exception^ exception)
			{
				page->m_libraryBusy = false;
				page->SetLibraryActionsEnabled(true);
				page->launchStatus->Text = "Could not copy the selected game file";
				page->AppendError("Game file copy failed: " +
					std::to_string(exception->HResult));
				page->UpdateStartButton();
			}
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::OpenGameFolder_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	auto picker = ref new FolderPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append("*");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFolderAsync()).then([weakThis](StorageFolder^ folder)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page || !folder) return;
		const auto main = page->m_main;
		page->BeginExternalLaunch([main, folder]()
		{
			return main && main->LaunchGame(folder);
		});
	}, task_continuation_context::use_current());
}

void DirectXPage::ImportKeys_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning)
		return;
	auto picker = ref new FileOpenPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append(".txt");
	Platform::WeakReference weakThis(this);
	create_task(picker->PickSingleFileAsync()).then([weakThis](StorageFile^ file)
	{
		if (!file) return;
		create_task(FileIO::ReadBufferAsync(file)).then([weakThis](task<IBuffer^> readTask)
		{
			auto page = weakThis.Resolve<DirectXPage>();
			if (!page) return;
			IBuffer^ buffer = nullptr;
			try
			{
				buffer = readTask.get();
			}
			catch (Platform::Exception^ exception)
			{
				page->launchStatus->Text = "keys.txt could not be read";
				page->AppendError("keys.txt read failed: " +
					std::to_string(exception->HResult));
				return;
			}
			if (!buffer || buffer->Length == 0) return;
			auto bytes = ref new Platform::Array<uint8_t>(buffer->Length);
			auto reader = DataReader::FromBuffer(buffer);
			reader->ReadBytes(bytes);
			std::vector<uint8_t> data(bytes->Data, bytes->Data + bytes->Length);
			uint32_t validKeys{};
			if (!page->m_main || !page->m_main->ImportKeys(data, &validKeys))
			{
				page->launchStatus->Text = "keys.txt was not imported";
				page->AppendError("The selected keys.txt contains no valid 128-bit keys or could not be saved.");
				return;
			}
			page->launchStatus->Text = FromUtf8(
				"keys.txt imported: " + std::to_string(validKeys) + " valid key(s)");
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
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
		std::vector<uint64_t> titleIds;
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
				if (main->SetGraphicPacksEnabledForTitle(titleId, true, &affected))
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
				page->toolTabs->SelectedIndex = 3;
				page->UpdateStartButton();
				return;
			}
			std::ostringstream status;
			status << result.first << " graphic pack(s) imported";
			if (result.second)
				status << "; " << result.second << " enabled";
			page->launchStatus->Text = FromUtf8(status.str());
			page->RefreshLibrary();
		}, task_continuation_context::use_current());
	}, task_continuation_context::use_current());
}

void DirectXPage::RefreshLibrary_Click(Platform::Object^, RoutedEventArgs^)
{
	RefreshLibrary(true);
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
	const auto& selectedTitle = m_installedTitles[selectedIndex];
	if (!selectedTitle.localGamePath.empty())
	{
		const auto main = m_main;
		const auto path = selectedTitle.localGamePath;
		BeginExternalLaunch([main, path]()
		{
			return main && main->LaunchGamePath(path);
		});
		return;
	}
	const uint64_t titleId = selectedTitle.titleId;
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
	SetGamePresentation(true);
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
		m_main->SetGraphicPacksEnabledForTitle(titleId, true, &enabledPacks);
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
			SetGamePresentation(false);
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

void DirectXPage::PlaceDimensionsFigure_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady)
		return;
	const int figureIndex = dimensionsFigureBox->SelectedIndex;
	const int slot = dimensionsSlotBox->SelectedIndex;
	if (figureIndex < 0 || figureIndex >= static_cast<int>(m_dimensionsFigures.size()) ||
		slot < 0 || slot >= 7)
	{
		dimensionsStatus->Text = "Choose both a figure and a Toy Pad position.";
		return;
	}

	const auto& figure = m_dimensionsFigures[figureIndex];
	if (!m_main->PlaceDimensionsFigure(figure.id, static_cast<uint8_t>(slot)))
	{
		dimensionsStatus->Text = "The virtual tag could not be placed. Check Help and errors.";
		AppendError("Could not create or place the LEGO Dimensions virtual tag.");
		return;
	}
	static constexpr const char* slotNames[] = {
		"Left: top", "Center", "Right: top", "Left: lower 1",
		"Left: lower 2", "Right: lower 1", "Right: lower 2"
	};
	dimensionsStatus->Text = FromUtf8(
		"Placed " + figure.name + " on " + slotNames[slot] + ".");
}

void DirectXPage::RemoveDimensionsFigure_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady)
		return;
	const int slot = dimensionsSlotBox->SelectedIndex;
	if (slot < 0 || slot >= 7)
	{
		dimensionsStatus->Text = "Choose a Toy Pad position first.";
		return;
	}
	if (!m_main->RemoveDimensionsFigure(static_cast<uint8_t>(slot)))
	{
		dimensionsStatus->Text = "That Toy Pad position is already empty.";
		return;
	}
	static constexpr const char* slotNames[] = {
		"Left: top", "Center", "Right: top", "Left: lower 1",
		"Left: lower 2", "Right: lower 1", "Right: lower 2"
	};
	dimensionsStatus->Text = FromUtf8(
		std::string("Removed the figure from ") + slotNames[slot] + ".");
}

void DirectXPage::MoveDimensionsFigure_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady)
		return;
	const int source = dimensionsSourceSlotBox->SelectedIndex;
	const int destination = dimensionsSlotBox->SelectedIndex;
	if (source < 0 || source >= 7 || destination < 0 || destination >= 7)
	{
		dimensionsStatus->Text = "Choose source and destination Toy Pad positions.";
		return;
	}
	if (!m_main->MoveDimensionsFigure(
		static_cast<uint8_t>(source), static_cast<uint8_t>(destination)))
	{
		dimensionsStatus->Text = "The source position is empty or the tag could not be moved.";
		return;
	}
	static constexpr const char* slotNames[] = {
		"Left: top", "Center", "Right: top", "Left: lower 1",
		"Left: lower 2", "Right: lower 1", "Right: lower 2"
	};
	dimensionsStatus->Text = FromUtf8(std::string("Moved the tag from ") +
		slotNames[source] + " to " + slotNames[destination] + ".");
}

void DirectXPage::RefreshDimensionsFigures()
{
	m_dimensionsFigures.clear();
	dimensionsFigureBox->Items->Clear();
	if (!m_main || !m_cemuReady)
		return;
	m_dimensionsFigures = m_main->GetDimensionsFigures();
	for (const auto& figure : m_dimensionsFigures)
	{
		const std::string type = figure.vehicleOrGadget ? "Vehicle/Gadget" : "Character";
		dimensionsFigureBox->Items->Append(
			FromUtf8(figure.name + "  [" + type + ", " + std::to_string(figure.id) + "]"));
	}
	const bool available = !m_dimensionsFigures.empty();
	dimensionsFigureBox->IsEnabled = available;
	dimensionsSlotBox->IsEnabled = available;
	dimensionsSourceSlotBox->IsEnabled = available;
	placeDimensionsFigureButton->IsEnabled = available;
	removeDimensionsFigureButton->IsEnabled = available;
	moveDimensionsFigureButton->IsEnabled = available;
	if (available)
	{
		dimensionsFigureBox->SelectedIndex = 0;
		dimensionsStatus->Text = "Native Toy Pad ready. Virtual tags are stored in the app's local data.";
	}
	else
		dimensionsStatus->Text = "The native LEGO Dimensions catalog is unavailable.";
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

void DirectXPage::BeginExternalLaunch(std::function<bool()> launchOperation)
{
	if (!m_main || !m_cemuReady || m_libraryBusy || m_gameRunning || !launchOperation)
		return;
	const auto gamepadState = PublishGamepadState();
	if (gamepadState.connected)
	{
		TryConfigureDefaultGamepad();
		if (!m_gamepadProfileReady)
		{
			launchStatus->Text = "Could not prepare the Xbox Controller profile";
			AppendError("The Wii U GamePad profile was not ready before starting the selected title.");
			return;
		}
	}

	m_libraryBusy = true;
	SetTabsVisible(false);
	SetGamePresentation(true);
	startButton->IsEnabled = false;
	SetLibraryActionsEnabled(false);
	SetSystemPointerForUi(false);
	m_main->SetVirtualMouse(0, 0, false, false);
	m_gameRunning = true;
	launchStatus->Text = "Preparing selected Wii U title...";
	emulatorPlaceholder->Visibility = CollapsedValue;
	FocusEmulatorInput();
	Platform::WeakReference weakThis(this);
	create_task(std::move(launchOperation)).then([weakThis](bool launched)
	{
		auto page = weakThis.Resolve<DirectXPage>();
		if (!page) return;
		if (launched)
		{
			page->launchStatus->Text = "Game running";
			page->FocusEmulatorInput();
			return;
		}
		page->m_gameRunning = false;
		page->m_libraryBusy = false;
		page->SetGamePresentation(false);
		page->SetSystemPointerForUi(true);
		page->SetTabsVisible(true);
		page->launchStatus->Text = "Could not start the selected Wii U title";
		page->AppendError("Cemu could not mount or launch the selected game format.");
		page->SetLibraryActionsEnabled(true);
		page->UpdateStartButton();
		page->emulatorPlaceholder->Visibility = VisibleValue;
	}, task_continuation_context::use_current());
}

void DirectXPage::RefreshLibrary(bool scanLocalInstallFolder)
{
	if (!m_main || !m_cemuReady || m_libraryBusy)
		return;
	m_libraryBusy = true;
	SetLibraryActionsEnabled(false);
	startButton->IsEnabled = false;
	launchStatus->Text = scanLocalInstallFolder
		? "Scanning LocalState\\GamesToInstall..."
		: "Refreshing library...";
	const auto main = m_main;
	create_task([main, scanLocalInstallFolder]()
	{
		LocalInstallScanResult scanResult{};
		try
		{
			auto importFolder = GetOrCreateLocalInstallFolder();
			if (importFolder && main)
			{
				std::vector<StorageFolder^> candidates;
				std::vector<StorageFolder^> graphicPackCandidates;
				FindLocalInstallContent(importFolder, candidates,
					scanResult.alreadyProcessed, graphicPackCandidates,
					scanResult.graphicPacksAlreadyProcessed,
					scanResult.localGameFiles);
				scanResult.discovered = static_cast<uint32_t>(candidates.size());
				scanResult.graphicPacksDiscovered =
					static_cast<uint32_t>(graphicPackCandidates.size());
				if (scanLocalInstallFolder)
				{
					for (const auto& candidate : candidates)
					{
						uint64_t baseTitleId{};
						if (!main->InstallTitle(candidate, CEMU_EMBED_INSTALL_AUTO,
							&baseTitleId))
						{
							++scanResult.failed;
							continue;
						}
						++scanResult.installed;
						try
						{
							auto marker = create_task(candidate->CreateFileAsync(
								WinRtString(LocalInstallMarkerName),
								CreationCollisionOption::ReplaceExisting)).get();
							create_task(FileIO::WriteTextAsync(marker,
								WinRtString(L"Installed by Cemu-UWP-Host. Delete this file to reinstall this source.\r\n"))).get();
						}
						catch (...)
						{
							// The title is already committed to the MLC. Report only the
							// missing marker, which can cause it to be offered again.
							++scanResult.markerWarnings;
						}
					}

					for (const auto& graphicPack : graphicPackCandidates)
					{
						uint32_t imported{};
						if (!main->InstallGraphicPacks(graphicPack, &imported) || !imported)
						{
							++scanResult.graphicPackFailures;
							continue;
						}
						scanResult.graphicPacksImported += imported;
						try
						{
							auto marker = create_task(graphicPack->CreateFileAsync(
								WinRtString(LocalGraphicPackMarkerName),
								CreationCollisionOption::ReplaceExisting)).get();
							create_task(FileIO::WriteTextAsync(marker,
								WinRtString(L"Imported by Cemu-UWP-Host. Delete this file to import this Graphic Pack again.\r\n"))).get();
						}
						catch (...)
						{
							++scanResult.markerWarnings;
						}
					}
				}
			}
		}
		catch (Platform::Exception^)
		{
			if (scanLocalInstallFolder)
				++scanResult.failed;
		}
		catch (...)
		{
			if (scanLocalInstallFolder)
				++scanResult.failed;
		}
		auto titles = main ? main->GetInstalledTitles() : std::vector<InstalledTitle>{};
		if (main)
		{
			for (const auto& title : titles)
			{
				uint32_t affected{};
				if (main->SetGraphicPacksEnabledForTitle(title.titleId, true,
					&affected))
					scanResult.enabledGraphicPacks += affected;
			}
			// Refresh the counters displayed by the library after changing pack
			// state for every installed title.
			titles = main->GetInstalledTitles();
		}
		return std::make_pair(std::move(titles), scanResult);
	}).then([this, scanLocalInstallFolder](
		std::pair<std::vector<InstalledTitle>, LocalInstallScanResult> result)
	{
		auto titles = std::move(result.first);
		const auto scanResult = result.second;
		for (const auto& localGame : scanResult.localGameFiles)
		{
			InstalledTitle localTitle{};
			localTitle.titleId = localGame.id;
			localTitle.name = localGame.name;
			localTitle.regionName = "Local storage";
			localTitle.localGamePath = localGame.path;
			localTitle.localGameFormat = localGame.format;
			titles.emplace_back(std::move(localTitle));
		}
		m_installedTitles = std::move(titles);
		installedGamesList->Items->Clear();
		for (const auto& title : m_installedTitles)
		{
			std::ostringstream line;
			line << title.name;
			if (!title.localGamePath.empty())
			{
				line << "\nLocal game file  |  Format: " << title.localGameFormat
					<< "  |  Ready to launch from GamesToInstall";
				installedGamesList->Items->Append(FromUtf8(line.str()));
				continue;
			}
			line << "\nTitle ID: " << std::uppercase << std::hex << std::setw(16)
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
		if (scanLocalInstallFolder)
		{
			std::ostringstream status;
			status << "Local scan complete";
			if (scanResult.installed)
				status << "; " << scanResult.installed << " title item(s) installed";
			if (!scanResult.localGameFiles.empty())
				status << "; " << scanResult.localGameFiles.size()
					<< " local game file(s) detected";
			if (!scanResult.discovered && !scanResult.graphicPacksDiscovered &&
				scanResult.localGameFiles.empty())
				status << "; no new content found";
			if (scanResult.alreadyProcessed)
				status << "; " << scanResult.alreadyProcessed << " already processed";
			if (scanResult.graphicPacksImported)
				status << "; " << scanResult.graphicPacksImported << " Graphic Pack(s) imported";
			if (scanResult.graphicPacksAlreadyProcessed)
				status << "; " << scanResult.graphicPacksAlreadyProcessed << " Graphic Pack(s) already processed";
			if (scanResult.enabledGraphicPacks)
				status << "; " << scanResult.enabledGraphicPacks << " Graphic Pack(s) enabled";
			if (scanResult.failed)
			{
				status << "; " << scanResult.failed << " failed";
				AppendError("Some content in LocalState\\GamesToInstall could not be installed. Each source must be an extracted base game, update, or DLC with code, content, and meta folders.");
			}
			if (scanResult.graphicPackFailures)
			{
				status << "; " << scanResult.graphicPackFailures << " Graphic Pack import(s) failed";
				AppendError("Some Graphic Packs in LocalState\\GamesToInstall could not be imported. Each pack folder must contain a valid rules.txt file.");
			}
			if (scanResult.markerWarnings)
			{
				status << "; " << scanResult.markerWarnings << " marker warning(s)";
				AppendError("Installed content could not be marked as processed and may be detected again on the next scan.");
			}
			launchStatus->Text = FromUtf8(status.str());
		}
		else
		{
			launchStatus->Text = m_installedTitles.empty()
				? "No games installed"
				: (restoredIndex >= 0 ? "Ready to start" : "Select an installed game");
		}
		UpdateStartButton();
	}, task_continuation_context::use_current());
}

void DirectXPage::SetLibraryActionsEnabled(bool enabled)
{
	const bool canUse = enabled && m_cemuReady;
	installContentButton->IsEnabled = canUse;
	openGameFileButton->IsEnabled = canUse && !m_gameRunning;
	openGameFolderButton->IsEnabled = canUse && !m_gameRunning;
	importKeysButton->IsEnabled = canUse && !m_gameRunning;
	refreshLibraryButton->IsEnabled = canUse;
	installGraphicPacksButton->IsEnabled = canUse;
	installedGamesList->IsEnabled = enabled;
}

void DirectXPage::ToggleTabs_Click(Platform::Object^, RoutedEventArgs^)
{
	SetTabsVisible(tabsPanel->Visibility != VisibleValue);
}

void DirectXPage::ToolTabs_SelectionChanged(
	Platform::Object^, Windows::UI::Xaml::Controls::SelectionChangedEventArgs^)
{
	// Library, Toy Pad and diagnostics are deliberately compact. Settings has
	// several categories and needs most of the available screen, with its inner
	// ScrollViewer handling the remaining content on both PC and Xbox.
	if (toolTabs->SelectedIndex != 2)
	{
		tabsPanel->MaxHeight = 420.0;
		return;
	}
	const double availableHeight = Window::Current
		? Window::Current->Bounds.Height - 112.0
		: 720.0;
	tabsPanel->MaxHeight = (std::max)(420.0, (std::min)(760.0, availableHeight));
}

void DirectXPage::ToggleGettingStarted_Click(Platform::Object^, RoutedEventArgs^)
{
	SetGettingStartedExpanded(!m_gettingStartedExpanded);
}

void DirectXPage::LoadSettings()
{
	CemuEmbedSettings settings{};
	if (!m_main || !m_main->GetSettings(settings))
	{
		applySettingsButton->IsEnabled = false;
		settingsStatus->Text = "Cemu settings are unavailable.";
		return;
	}
	auto select = [](ComboBox^ box, int value, int maximum)
	{
		box->SelectedIndex = (std::max)(0, (std::min)(value, maximum));
	};
	select(cpuModeBox, settings.cpu_mode, 4);
	select(consoleLanguageBox, settings.console_language, 11);
	select(vsyncBox, settings.vsync == 0 ? 0 : 1, 1);
	bootSoundCheck->IsChecked = settings.play_boot_sound != 0;
	disableScreensaverCheck->IsChecked = settings.disable_screensaver != 0;
	asyncCompileCheck->IsChecked = settings.async_compile != 0;
	gx2SyncCheck->IsChecked = settings.gx2drawdone_sync != 0;
	upsideDownCheck->IsChecked = settings.render_upside_down != 0;
	select(upscaleFilterBox, settings.upscale_filter, 3);
	select(downscaleFilterBox, settings.downscale_filter, 3);
	select(scalingBox, settings.fullscreen_scaling, 1);
	overrideGammaCheck->IsChecked = settings.override_gamma != 0;
	gammaSlider->Value = settings.override_gamma_value;
	displayGammaSlider->Value = settings.display_gamma;
	select(notificationPositionBox, settings.notification_position, 6);
	notificationScaleSlider->Value = settings.notification_text_scale;
	notifyProfilesCheck->IsChecked = settings.notification_controller_profiles != 0;
	notifyBatteryCheck->IsChecked = settings.notification_controller_battery != 0;
	notifyShadersCheck->IsChecked = settings.notification_shader_compiling != 0;
	notifyFriendsCheck->IsChecked = settings.notification_friends != 0;
	// The UI order is optimized for UWP, but settings.xml stores IAudioAPI's
	// native enum values: DirectSound=0, XAudio27=1, XAudio2=2, Cubeb=3.
	const int audioApiIndex = settings.audio_api == 0 ? 1 :
		settings.audio_api == 1 ? 2 :
		settings.audio_api == 3 ? 3 : 0;
	select(audioApiBox, audioApiIndex, 3);
	audioDelaySlider->Value = settings.audio_delay;
	select(tvChannelsBox, settings.tv_channels, 2);
	select(padChannelsBox, settings.pad_channels, 2);
	select(inputChannelsBox, settings.input_channels, 2);
	tvVolumeSlider->Value = settings.tv_volume;
	padVolumeSlider->Value = settings.pad_volume;
	inputVolumeSlider->Value = settings.input_volume;
	portalVolumeSlider->Value = settings.portal_volume;
	skylandersCheck->IsChecked = settings.emulate_skylander_portal != 0;
	infinityCheck->IsChecked = settings.emulate_infinity_base != 0;
	dimensionsCheck->IsChecked = settings.emulate_dimensions_toypad != 0;
	applySettingsButton->IsEnabled = !m_gameRunning;
	settingsStatus->Text = "Settings loaded. Select Apply settings to save changes.";
}

void DirectXPage::ApplySettings_Click(Platform::Object^, RoutedEventArgs^)
{
	if (!m_main || !m_cemuReady || m_gameRunning)
	{
		settingsStatus->Text = "Stop the running game before changing settings.";
		return;
	}
	CemuEmbedSettings settings{};
	if (!m_main->GetSettings(settings))
	{
		settingsStatus->Text = "Could not read the current Cemu settings.";
		return;
	}
	auto checked = [](CheckBox^ box) { return box->IsChecked->Value ? 1 : 0; };
	settings.cpu_mode = cpuModeBox->SelectedIndex;
	settings.console_language = consoleLanguageBox->SelectedIndex;
	settings.vsync = vsyncBox->SelectedIndex;
	settings.play_boot_sound = checked(bootSoundCheck);
	settings.disable_screensaver = checked(disableScreensaverCheck);
	settings.async_compile = checked(asyncCompileCheck);
	settings.gx2drawdone_sync = checked(gx2SyncCheck);
	settings.render_upside_down = checked(upsideDownCheck);
	settings.upscale_filter = upscaleFilterBox->SelectedIndex;
	settings.downscale_filter = downscaleFilterBox->SelectedIndex;
	settings.fullscreen_scaling = scalingBox->SelectedIndex;
	settings.override_gamma = checked(overrideGammaCheck);
	settings.override_gamma_value = static_cast<float>(gammaSlider->Value);
	settings.display_gamma = static_cast<float>(displayGammaSlider->Value);
	settings.notification_position = notificationPositionBox->SelectedIndex;
	settings.notification_text_scale = static_cast<int32_t>(notificationScaleSlider->Value);
	settings.notification_controller_profiles = checked(notifyProfilesCheck);
	settings.notification_controller_battery = checked(notifyBatteryCheck);
	settings.notification_shader_compiling = checked(notifyShadersCheck);
	settings.notification_friends = checked(notifyFriendsCheck);
	static constexpr int32_t audioApiValues[] = { 2, 0, 1, 3 };
	const int audioApiIndex = (std::max)(0,
		(std::min)(audioApiBox->SelectedIndex, 3));
	settings.audio_api = audioApiValues[audioApiIndex];
	settings.audio_delay = static_cast<int32_t>(audioDelaySlider->Value);
	settings.tv_channels = tvChannelsBox->SelectedIndex;
	settings.pad_channels = padChannelsBox->SelectedIndex;
	settings.input_channels = inputChannelsBox->SelectedIndex;
	settings.tv_volume = static_cast<int32_t>(tvVolumeSlider->Value);
	settings.pad_volume = static_cast<int32_t>(padVolumeSlider->Value);
	settings.input_volume = static_cast<int32_t>(inputVolumeSlider->Value);
	settings.portal_volume = static_cast<int32_t>(portalVolumeSlider->Value);
	settings.emulate_skylander_portal = checked(skylandersCheck);
	settings.emulate_infinity_base = checked(infinityCheck);
	settings.emulate_dimensions_toypad = checked(dimensionsCheck);
	if (!m_main->SetSettings(settings))
	{
		settingsStatus->Text = "Could not save Cemu settings.";
		return;
	}
	settingsStatus->Text = "Settings saved. USB and startup options apply after restarting the app.";
}

void DirectXPage::SetGettingStartedExpanded(bool expanded)
{
	m_gettingStartedExpanded = expanded;
	gettingStartedDetails->Visibility = expanded ? VisibleValue : CollapsedValue;
	gettingStartedToggleText->Text = expanded ? "Hide guide" : "Show guide";
	// Segoe MDL2 Assets: ChevronUp / ChevronDown.
	gettingStartedToggleIcon->Glyph = WinRtString(expanded ? L"\xE70E" : L"\xE70D");
	Windows::UI::Xaml::Automation::AutomationProperties::SetName(
		gettingStartedToggleButton,
		WinRtString(expanded ? L"Hide getting started guide" : L"Show getting started guide"));
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
	// While a title owns the surface, no XAML chrome may cover or resize it.
	// Options become available again only after leaving the game presentation.
	if (m_gameRunning && visible)
		visible = false;
	tabsPanel->Visibility = visible ? VisibleValue : CollapsedValue;
	toggleTabsButtonText->Text = visible ? "Hide options" : "Show options";
	if (!visible && m_gameRunning)
		FocusEmulatorInput();
}

void DirectXPage::SetGamePresentation(bool running)
{
	applySettingsButton->IsEnabled = m_cemuReady && !running;
	if (running)
	{
		topCommandBar->Visibility = CollapsedValue;
		tabsPanel->Visibility = CollapsedValue;
		Grid::SetRow(emulatorViewport, 0);
		Grid::SetRowSpan(emulatorViewport, 2);
		return;
	}

	// Restore every focus target that FocusEmulatorInput disables. Without this,
	// a failed launch leaves the library visible but impossible to navigate with
	// the Xbox controller. Returning from game presentation must also restore the
	// tools panel that was hidden immediately before launch.
	toggleTabsButton->IsTabStop = true;
	startButton->IsTabStop = true;
	metricsButton->IsTabStop = true;
	topCommandBar->Visibility = VisibleValue;
	tabsPanel->Visibility = VisibleValue;
	toggleTabsButtonText->Text = "Hide options";
	Grid::SetRow(emulatorViewport, 1);
	Grid::SetRowSpan(emulatorViewport, 1);
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
				RefreshDimensionsFigures();
				LoadSettings();
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
				SetGamePresentation(false);
				m_performanceMetricsVisible = false;
				metricsButtonText->Text = "Show metrics";
				SetSystemPointerForUi(true);
				if (m_virtualMouseEnabled)
					SetVirtualMouseEnabled(false);
				SetLibraryActionsEnabled(false);
				dimensionsFigureBox->IsEnabled = false;
				dimensionsSlotBox->IsEnabled = false;
				dimensionsSourceSlotBox->IsEnabled = false;
				placeDimensionsFigureButton->IsEnabled = false;
				removeDimensionsFigureButton->IsEnabled = false;
				moveDimensionsFigureButton->IsEnabled = false;
				applySettingsButton->IsEnabled = false;
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
	if (!m_gameRunning || !m_gamepadProfileReady || !gamepad.connected ||
		tabsPanel->Visibility == VisibleValue)
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
	// Do not let UI navigation also control the running Wii U title. Keep the
	// device connected so Cemu does not rebuild its input topology; only publish
	// a neutral reading while the options panel has focus.
	auto publishedState = state;
	if (m_gameRunning && tabsPanel->Visibility == VisibleValue)
	{
		publishedState.buttons = 0;
		publishedState.left_x = 0.0f;
		publishedState.left_y = 0.0f;
		publishedState.right_x = 0.0f;
		publishedState.right_y = 0.0f;
		publishedState.left_trigger = 0.0f;
		publishedState.right_trigger = 0.0f;
	}
	if (!m_hasPublishedGamepadState ||
		std::memcmp(&publishedState, &m_lastPublishedGamepadState, sizeof(publishedState)) != 0)
	{
		m_main->SetGamepadState(publishedState);
		m_lastPublishedGamepadState = publishedState;
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

void DirectXPage::SaveInternalState(Windows::Foundation::Collections::IPropertySet^ state)
{
	(void)state;
}

void DirectXPage::LoadInternalState(Windows::Foundation::Collections::IPropertySet^ state)
{
	(void)state;
	SetGettingStartedExpanded(false);
}
