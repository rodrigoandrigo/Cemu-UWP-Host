//
// App.xaml.cpp
// Implementação da classe App.
//

#include "pch.h"
#include "DirectXPage.xaml.h"

using namespace Cemu_UWP_Host;

using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Storage;
using namespace Windows::System::Profile;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Interop;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::ViewManagement;

namespace
{
bool IsXboxDevice()
{
	try
	{
		auto versionInfo = AnalyticsInfo::VersionInfo;
		return versionInfo != nullptr && versionInfo->DeviceFamily == "Windows.Xbox";
	}
	catch (Platform::Exception^)
	{
		// If platform detection is unavailable, keep the desktop-safe windowed
		// behavior instead of forcing an unsupported display transition.
		return false;
	}
}
}
/// <summary>
/// Inicializa o objeto singleton do aplicativo.  Esta é a primeira linha de código criado
/// executado e, como tal, é o equivalente lógico de main() ou WinMain().
/// </summary>
App::App()
{
	InitializeComponent();
	// Keep the platform cursor available for library navigation. DirectXPage
	// suppresses it only while a title owns the emulator presentation.
	RequiresPointerMode = ApplicationRequiresPointerMode::Auto;
	Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
	Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);
}

/// <summary>
/// Chamado quando o aplicativo é iniciado normalmente pelo usuário final.  Outros pontos de entrada
/// serão usados quando o aplicativo é iniciado para abrir um arquivo específico, para exibir
/// resultados da pesquisa e assim por diante.
/// </summary>
/// <param name="e">Detalhes sobre a solicitação e o processo de inicialização.</param>
void App::OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e)
{
#if defined(_DEBUG) && defined(CEMU_UWP_ENABLE_XAML_FPS_COUNTER)
	if (IsDebuggerPresent())
	{
		DebugSettings->EnableFrameRateCounter = true;
	}
#endif

	auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);

	// Não repita a inicialização do aplicativo quando a Janela já tiver conteúdo,
	// apenas verifique se a janela está ativa
	if (rootFrame == nullptr)
	{
		// Criar um Quadro para agir como o contexto de navegação e associá-lo a
		// uma chave SuspensionManager
		rootFrame = ref new Frame();

		rootFrame->NavigationFailed += ref new Windows::UI::Xaml::Navigation::NavigationFailedEventHandler(this, &App::OnNavigationFailed);

		// Coloque o quadro na Janela atual
		Window::Current->Content = rootFrame;
	}

	if (rootFrame->Content == nullptr)
	{
		// Quando a pilha de navegação não for restaurada, navegar para a primeira página,
		// configurando a nova página passando as informações necessárias como um parâmetro
		// parâmetro
		rootFrame->Navigate(TypeName(DirectXPage::typeid), e->Arguments);
	}

	if (m_directXPage == nullptr)
	{
		m_directXPage = dynamic_cast<DirectXPage^>(rootFrame->Content);
	}

	if (e->PreviousExecutionState == ApplicationExecutionState::Terminated)
	{
		m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	}
	
	// Verifique se a janela atual está ativa
	Window::Current->Activate();
	EnterFullScreen();
}

void App::EnterFullScreen()
{
	try
	{
		auto view = ApplicationView::GetForCurrentView();
		if (!IsXboxDevice())
		{
			// Full-screen startup is an Xbox presentation policy. Desktop UWP must
			// retain the normal resizable window selected by Windows.
			if (view->IsFullScreenMode)
				view->ExitFullScreenMode();
			return;
		}
		// Xbox normally reserves a 5% TV-safe margin on each side for XAML. Use the
		// CoreWindow bounds so the host, command bar and SwapChainPanel fill the
		// complete display instead of rendering as a centered 1728x972 surface.
		view->SetDesiredBoundsMode(ApplicationViewBoundsMode::UseCoreWindow);
		if (!view->IsFullScreenMode)
			view->TryEnterFullScreenMode();
	}
	catch (Platform::Exception^)
	{
		// Some desktop/window-management policies may reject the request. The app
		// remains usable with the bounds selected by the system in that case.
	}
}
/// <summary>
/// Chamado quando a execução do aplicativo está sendo suspensa.  O estado do aplicativo é salvo
/// sem saber se o aplicativo será encerrado ou retomado com o conteúdo
/// da memória ainda intacto.
/// </summary>
/// <param name="sender">A fonte da solicitação de suspensão.</param>
/// <param name="e">Detalhes sobre a solicitação de suspensão.</param>
void App::OnSuspending(Object^ sender, SuspendingEventArgs^ e)
{
	(void) sender;	// Parâmetro não usado
	(void) e;	// Parâmetro não usado

	m_directXPage->SaveInternalState(ApplicationData::Current->LocalSettings->Values);
}

/// <summary>
/// Invocado quando a execução do aplicativo é retomada.
/// </summary>
/// <param name="sender">A origem da solicitação de retomada.</param>
/// <param name="args">Detalhes sobre a solicitação de retomada.</param>
void App::OnResuming(Object ^sender, Object ^args)
{
	(void) sender; // Parâmetro não usado
	(void) args; // Parâmetro não usado

	m_directXPage->LoadInternalState(ApplicationData::Current->LocalSettings->Values);
	EnterFullScreen();
}

/// <summary>
/// Chamado quando ocorre uma falha na Navegação para uma determinada página
/// </summary>
/// <param name="sender">O Quadro com navegação com falha</param>
/// <param name="e">Detalhes sobre a falha na navegação</param>
void App::OnNavigationFailed(Platform::Object ^sender, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs ^e)
{
	throw ref new FailureException("Failed to load Page " + e->SourcePageType.Name);
}

