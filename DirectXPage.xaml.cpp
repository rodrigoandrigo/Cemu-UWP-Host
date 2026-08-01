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

double ApplyStickDeadzone(double value)
{
	constexpr double deadzone = 0.18;
	const double magnitude = std::abs(value);
	if (magnitude <= deadzone)
		return 0.0;
	return std::copysign((magnitude - deadzone) / (1.0 - deadzone), value);
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
	// Registering these WGI events is required for reliable Xbox gamepad
	// discovery. SDL3-UWP consumes the controller state and rumble requests.
	m_gamepadAddedToken =
		Gamepad::GamepadAdded += ref new EventHandler<Gamepad^>(this, &DirectXPage::OnGamepadAdded);
	m_gamepadRemovedToken =
		Gamepad::GamepadRemoved += ref new EventHandler<Gamepad^>(this, &DirectXPage::OnGamepadRemoved);
	m_rawControllerAddedToken =
		RawGameController::RawGameControllerAdded +=
		ref new EventHandler<RawGameController^>(this, &DirectXPage::OnRawGameControllerAdded);
	m_rawControllerRemovedToken =
		RawGameController::RawGameControllerRemoved +=
		ref new EventHandler<RawGameController^>(this, &DirectXPage::OnRawGameControllerRemoved);
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
	m_deviceResources = std::make_shared<DX::DeviceResources>();
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
				m_main = std::make_unique<Cemu_UWP_HostMain>(
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
				AppendError("Falha ao inicializar o Cemu ou o backend Direct3D 11.");
				launchStatus->Text = "Falha ao inicializar o emulador";
				m_main.reset();
			}
		}
	}
}

DirectXPage::~DirectXPage()
{
	CompositionTarget::Rendering -= m_renderingToken;
	Gamepad::GamepadAdded -= m_gamepadAddedToken;
	Gamepad::GamepadRemoved -= m_gamepadRemovedToken;
	RawGameController::RawGameControllerAdded -= m_rawControllerAddedToken;
	RawGameController::RawGameControllerRemoved -= m_rawControllerRemovedToken;
	if (m_main)
		m_main->SetVirtualMouse(0, 0, false, false);
	m_main.reset();
}

void DirectXPage::OnRendering(Platform::Object^, Platform::Object^)
{
	if (m_main) m_main->Pump();
	UpdateVirtualMouse();
	if (++m_controllerPollFrames >= 60)
	{
		m_controllerPollFrames = 0;
		UpdateGamepadStatus();
	}
	if (m_cemuReady && !m_gamepadProfileReady && Gamepad::Gamepads->Size > 0 &&
		++m_gamepadRetryFrames >= 60)
	{
		m_gamepadRetryFrames = 0;
		TryConfigureDefaultGamepad();
	}
}

void DirectXPage::OnGamepadAdded(Platform::Object^, Gamepad^)
{
	// SDL3-UWP receives the device through Windows.Gaming.Input and emits its
	// normal SDL gamepad-added event for Cemu's SDLControllerProvider.
	m_gamepadProfileReady = false;
	m_gamepadRetryFrames = 59;
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this]() { UpdateGamepadStatus(); })));
}

void DirectXPage::OnGamepadRemoved(Platform::Object^, Gamepad^)
{
	// SDL3-UWP emits the matching removal event and releases the controller.
	m_gamepadProfileReady = false;
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this]()
		{
			if (m_virtualMouseEnabled)
				SetVirtualMouseEnabled(false);
			else
				UpdateGamepadStatus();
		})));
}

void DirectXPage::OnRawGameControllerAdded(Platform::Object^, RawGameController^)
{
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this]() { UpdateGamepadStatus(); })));
}

void DirectXPage::OnRawGameControllerRemoved(Platform::Object^, RawGameController^)
{
	create_task(Dispatcher->RunAsync(CoreDispatcherPriority::Normal,
		ref new DispatchedHandler([this]() { UpdateGamepadStatus(); })));
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
	create_task(picker->PickSingleFolderAsync()).then([this](StorageFolder^ folder)
	{
		if (!folder) return;
		m_libraryBusy = true;
		SetLibraryActionsEnabled(false);
		startButton->IsEnabled = false;
		launchStatus->Text = "Importando graphic packs...";
		const int selectedIndex = installedGamesList->SelectedIndex;
		const uint64_t selectedTitleId =
			selectedIndex >= 0 && static_cast<size_t>(selectedIndex) < m_installedTitles.size()
				? m_installedTitles[selectedIndex].titleId : uint64_t{};
		std::vector<uint64_t> titleIds;
		if (selectedTitleId)
			titleIds.push_back(selectedTitleId);
		else
			for (const auto& title : m_installedTitles)
				titleIds.push_back(title.titleId);
		create_task([this, folder, titleIds]()
		{
			uint32_t imported{};
			if (!m_main || !m_main->InstallGraphicPacks(folder, &imported))
				return std::pair<uint32_t, uint32_t>{};
			uint32_t enabled{};
			for (const auto titleId : titleIds)
			{
				uint32_t affected{};
				if (m_main->ApplySafeGraphicPackPolicyForTitle(titleId, &affected))
					enabled += affected;
			}
			return std::make_pair(imported, enabled);
		}).then([this](std::pair<uint32_t, uint32_t> result)
		{
			m_libraryBusy = false;
			SetLibraryActionsEnabled(true);
			if (!result.first)
			{
				launchStatus->Text = "Falha ao importar graphic packs; veja a aba Erros";
				SetTabsVisible(true);
				toolTabs->SelectedIndex = 1;
				UpdateStartButton();
				return;
			}
			std::ostringstream status;
			status << result.first << " graphic pack(s) importado(s)";
			if (result.second)
				status << "; " << result.second << " ajustado(s) pela política segura";
			launchStatus->Text = FromUtf8(status.str());
			RefreshLibrary();
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
	if (installedGamesList->SelectedIndex >= 0 &&
		static_cast<size_t>(installedGamesList->SelectedIndex) < m_installedTitles.size())
		launchStatus->Text = "Pronto para iniciar";
	UpdateStartButton();
}

void DirectXPage::StartGame_Click(Platform::Object^, RoutedEventArgs^)
{
	const int selectedIndex = installedGamesList->SelectedIndex;
	if (!m_main || !m_main->IsReady() || selectedIndex < 0 ||
		static_cast<size_t>(selectedIndex) >= m_installedTitles.size())
		return;
	const uint64_t titleId = m_installedTitles[selectedIndex].titleId;
	SetTabsVisible(false);
	startButton->IsEnabled = false;
	SetLibraryActionsEnabled(false);
	launchStatus->Text = "Montando jogo, atualização e DLC...";
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
			launchStatus->Text = "Jogo em execução";
			FocusEmulatorInput();
		}
		else
		{
			m_gameRunning = false;
			launchStatus->Text = "Não foi possível iniciar o jogo instalado";
			AppendError("Não foi possível montar o jogo base com a atualização e o DLC instalados.");
			SetLibraryActionsEnabled(true);
			UpdateStartButton();
			emulatorPlaceholder->Visibility = VisibleValue;
		}
	}, task_continuation_context::use_current());
}

void DirectXPage::BeginInstall()
{
	if (!m_main || !m_cemuReady || m_libraryBusy)
		return;
	auto picker = ref new FolderPicker();
	picker->SuggestedStartLocation = PickerLocationId::ComputerFolder;
	picker->FileTypeFilter->Append("*");
	create_task(picker->PickSingleFolderAsync()).then(
		[this](StorageFolder^ folder)
	{
		if (!folder) return;
		m_libraryBusy = true;
		SetLibraryActionsEnabled(false);
		startButton->IsEnabled = false;
		launchStatus->Text = "Instalando conteúdo...";
		create_task([this, folder]()
		{
			uint64_t baseTitleId{};
			const bool installed = m_main &&
				m_main->InstallTitle(folder, CEMU_EMBED_INSTALL_AUTO, &baseTitleId);
			return installed ? baseTitleId : uint64_t{};
		}).then([this](uint64_t installedBaseTitleId)
		{
			m_libraryBusy = false;
			SetLibraryActionsEnabled(true);
			if (!installedBaseTitleId)
			{
				launchStatus->Text = "Falha na instalação";
				UpdateStartButton();
				return;
			}
			launchStatus->Text = "Instalação concluída";
			RefreshLibrary();
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
	launchStatus->Text = "Atualizando biblioteca...";
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
				<< "  |  Versão: v" << title.effectiveVersion
				<< " (base v" << title.baseVersion;
			if (title.updateVersion)
				line << ", atualização v" << title.updateVersion;
			else
				line << ", sem atualização";
			line << ")  |  DLC: ";
			if (title.dlcCount)
				line << title.dlcCount << " instalado(s), v" << title.dlcVersion;
			else
				line << "não instalado";
			line << "  |  Região: " << title.regionName
				<< "  |  Graphic packs: " << title.enabledGraphicPackCount
				<< "/" << title.compatibleGraphicPackCount << " ativos";
			installedGamesList->Items->Append(FromUtf8(line.str()));
		}
		m_libraryBusy = false;
		SetLibraryActionsEnabled(true);
		launchStatus->Text = m_installedTitles.empty()
			? "Nenhum jogo instalado"
			: "Selecione um jogo instalado";
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
	this->Focus(Windows::UI::Xaml::FocusState::Programmatic);
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
	toggleTabsButton->Label = visible ? "Ocultar abas" : "Mostrar abas";
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
			if (state == CEMU_EMBED_STATE_READY)
			{
				launchStatus->Text = "Carregando biblioteca...";
				SetLibraryActionsEnabled(true);
				m_gamepadRetryFrames = 59;
				UpdateActiveAccount();
				TryConfigureDefaultGamepad();
				RefreshLibrary();
			}
			else if (state == CEMU_EMBED_STATE_INITIALIZING)
				launchStatus->Text = "Emulador inicializando...";
			else if (state == CEMU_EMBED_STATE_FAILED)
			{
				launchStatus->Text = "Falha ao inicializar o emulador";
				accountStatus->Text = "Conta indisponível";
			}
			if (state != CEMU_EMBED_STATE_READY)
			{
				m_gameRunning = false;
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
		if (m_virtualMouseX <= 0.0 && m_virtualMouseY <= 0.0)
		{
			m_virtualMouseX = width * 0.5;
			m_virtualMouseY = height * 0.5;
		}
		m_virtualMouseX = (std::min)((std::max)(m_virtualMouseX, 0.0), (std::max)(width - 1.0, 0.0));
		m_virtualMouseY = (std::min)((std::max)(m_virtualMouseY, 0.0), (std::max)(height - 1.0, 0.0));
		virtualMouseTransform->X = m_virtualMouseX - 2.0;
		virtualMouseTransform->Y = m_virtualMouseY - 2.0;
		virtualMouseCursor->Visibility = VisibleValue;

		const double scaleX = emulatorSurface->CompositionScaleX > 0.0f
			? emulatorSurface->CompositionScaleX : 1.0;
		const double scaleY = emulatorSurface->CompositionScaleY > 0.0f
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

void DirectXPage::UpdateVirtualMouse()
{
	if (!m_gameRunning || !m_gamepadProfileReady || Gamepad::Gamepads->Size == 0)
	{
		m_virtualMouseChordHeld = false;
		if (m_virtualMouseEnabled)
			SetVirtualMouseEnabled(false);
		return;
	}

	try
	{
		const auto reading = Gamepad::Gamepads->GetAt(0)->GetCurrentReading();
		const bool chord =
			HasGamepadButton(reading.Buttons, GamepadButtons::LeftShoulder) &&
			HasGamepadButton(reading.Buttons, GamepadButtons::RightShoulder);
		if (chord && !m_virtualMouseChordHeld)
			SetVirtualMouseEnabled(!m_virtualMouseEnabled);
		m_virtualMouseChordHeld = chord;

		const auto now = std::chrono::steady_clock::now();
		if (!m_virtualMouseEnabled)
		{
			m_virtualMouseLastUpdate = now;
			return;
		}

		double elapsed = std::chrono::duration<double>(now - m_virtualMouseLastUpdate).count();
		m_virtualMouseLastUpdate = now;
		elapsed = (std::min)((std::max)(elapsed, 0.0), 0.05);
		constexpr double cursorSpeed = 900.0;
		const double width = emulatorViewport->ActualWidth;
		const double height = emulatorViewport->ActualHeight;
		m_virtualMouseX += ApplyStickDeadzone(reading.LeftThumbstickX) * cursorSpeed * elapsed;
		m_virtualMouseY -= ApplyStickDeadzone(reading.LeftThumbstickY) * cursorSpeed * elapsed;
		m_virtualMouseX = (std::min)((std::max)(m_virtualMouseX, 0.0), (std::max)(width - 1.0, 0.0));
		m_virtualMouseY = (std::min)((std::max)(m_virtualMouseY, 0.0), (std::max)(height - 1.0, 0.0));
		virtualMouseTransform->X = m_virtualMouseX - 2.0;
		virtualMouseTransform->Y = m_virtualMouseY - 2.0;

		m_virtualMouseLeftDown = HasGamepadButton(reading.Buttons, GamepadButtons::A);
		const double scaleX = emulatorSurface->CompositionScaleX > 0.0f
			? emulatorSurface->CompositionScaleX : 1.0;
		const double scaleY = emulatorSurface->CompositionScaleY > 0.0f
			? emulatorSurface->CompositionScaleY : 1.0;
		if (m_main)
			m_main->SetVirtualMouse(
				static_cast<int>(std::lround(m_virtualMouseX * scaleX)),
				static_cast<int>(std::lround(m_virtualMouseY * scaleY)),
				m_virtualMouseLeftDown, true);
	}
	catch (...)
	{
		m_virtualMouseChordHeld = false;
		if (m_virtualMouseEnabled)
			SetVirtualMouseEnabled(false);
	}
}

void DirectXPage::UpdateGamepadStatus()
{
	const unsigned int gamepadCount = Gamepad::Gamepads->Size;
	const unsigned int rawCount = RawGameController::RawGameControllers->Size;
	if (!gamepadCount && !rawCount)
	{
		controllerStatus->Text = "Controle desconectado";
		controllerStatus->Opacity = 0.65;
		m_gamepadProfileReady = false;
		return;
	}
	std::wostringstream text;
	if (gamepadCount)
	{
		text << L"Controle Xbox conectado";
		if (gamepadCount > 1)
			text << L" (" << gamepadCount << L")";
		if (m_gamepadProfileReady)
		{
			text << L" \u2022 perfil Wii U GamePad";
			if (m_virtualMouseEnabled)
				text << L" \u2022 mouse ativo (A: clique; L+R: fechar)";
			else if (m_gameRunning)
				text << L" \u2022 L+R: mouse";
		}
		else
			text << L" \u2022 preparando perfil";
	}
	else
	{
		text << L"Controle conectado";
		if (rawCount > 1)
			text << L" (" << rawCount << L")";
		text << L" \u2022 modo genérico";
	}
	controllerStatus->Text = ref new Platform::String(text.str().c_str());
	controllerStatus->Opacity = 1.0;
}

void DirectXPage::UpdateActiveAccount()
{
	ActiveAccount account;
	if (!m_main || !m_main->GetActiveAccount(account))
	{
		accountStatus->Text = "Conta indisponível";
		accountStatus->Opacity = 0.65;
		return;
	}

	std::ostringstream text;
	text << "Conta: " << (account.miiName.empty() ? "default" : account.miiName)
		<< " (" << std::uppercase << std::hex << std::setw(8)
		<< std::setfill('0') << account.persistentId << ")";
	if (account.onlineEnabled)
		text << " \xE2\x80\xA2 online";
	accountStatus->Text = FromUtf8(text.str());
	accountStatus->Opacity = 1.0;
}

void DirectXPage::TryConfigureDefaultGamepad()
{
	if (!m_main || !m_cemuReady || Gamepad::Gamepads->Size == 0)
		return;
	m_gamepadProfileReady = m_main->EnsureDefaultGamepadProfile();
	UpdateGamepadStatus();
}

void DirectXPage::OnBrokeredProgress(uint64_t bytesCopied, uint64_t totalBytes, const std::string&)
{
	std::ostringstream status;
	const double copiedGiB = static_cast<double>(bytesCopied) / (1024.0 * 1024.0 * 1024.0);
	const double totalGiB = static_cast<double>(totalBytes) / (1024.0 * 1024.0 * 1024.0);
	const double percent = totalBytes ? (100.0 * static_cast<double>(bytesCopied) / static_cast<double>(totalBytes)) : 0.0;
	status << "Copiando título: " << std::fixed << std::setprecision(1) << percent
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
	const int selectedIndex = installedGamesList->SelectedIndex;
	startButton->IsEnabled = m_main != nullptr && m_cemuReady && !m_libraryBusy &&
		selectedIndex >= 0 &&
		static_cast<size_t>(selectedIndex) < m_installedTitles.size();
}

void DirectXPage::SaveInternalState(Windows::Foundation::Collections::IPropertySet^)
{
}

void DirectXPage::LoadInternalState(Windows::Foundation::Collections::IPropertySet^)
{
}
