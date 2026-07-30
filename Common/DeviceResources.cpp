#include "pch.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include <windows.ui.xaml.media.dxinterop.h>
#include <algorithm>

using namespace D2D1;
using namespace DirectX;
using namespace Microsoft::WRL;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml::Controls;
using namespace Platform;

namespace DisplayMetrics
{
	// Monitores de alta resolução podem exigir muita energia da bateria e GPU para renderização.
	// Telefones de alta resolução, por exemplo, talvez tenham problemas de vida útil curta da bateria se
	// jogos tentam renderizar em 60 quadros por segundo com total fidelidade.
	// A decisão de renderizar com fidelidade total em todas as plataformas e fatores forma
	// deve ser proposital.
	static const bool SupportHighResolutions = false;

	// Os limites padrão que definem uma exibição de "alta resolução". Se os limites
	// forem excedidos e SupportHighResolutions for false, as dimensões serão escaladas
	// em 50%.
	static const float DpiThreshold = 192.0f;		// 200% da exibição padrão da área de trabalho.
	static const float WidthThreshold = 1920.0f;	// Largura de 1080p.
	static const float HeightThreshold = 1080.0f;	// Altura de 1080p.
};

// Constantes usadas para calcular as rotações de tela.
namespace ScreenRotation
{
	// Rotação Z de 0 grau
	static const XMFLOAT4X4 Rotation0( 
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
		);

	// Rotação Z de 90 graus
	static const XMFLOAT4X4 Rotation90(
		0.0f, 1.0f, 0.0f, 0.0f,
		-1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
		);

	// Rotação Z de 180 graus
	static const XMFLOAT4X4 Rotation180(
		-1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, -1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
		);

	// Rotação Z de 270 graus
	static const XMFLOAT4X4 Rotation270( 
		0.0f, -1.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
		);
};

// Construtor para DeviceResources.
DX::DeviceResources::DeviceResources() : 
	m_screenViewport(),
	m_d3dFeatureLevel(D3D_FEATURE_LEVEL_9_1),
	m_d3dRenderTargetSize(),
	m_outputSize(),
	m_logicalSize(),
	m_nativeOrientation(DisplayOrientations::None),
	m_currentOrientation(DisplayOrientations::None),
	m_dpi(-1.0f),
	m_effectiveDpi(-1.0f),
	m_compositionScaleX(1.0f),
	m_compositionScaleY(1.0f),
	m_deviceNotify(nullptr)
{
	CreateDeviceIndependentResources();
	CreateDeviceResources();
}

// Configura os recursos que não dependem do dispositivo Direct3D.
void DX::DeviceResources::CreateDeviceIndependentResources()
{
	// Inicializa os recursos do Direct2D.
	D2D1_FACTORY_OPTIONS options;
	ZeroMemory(&options, sizeof(D2D1_FACTORY_OPTIONS));

#if defined(_DEBUG)
	// Se o projeto estiver em uma compilação de depuração, habilite a depuração do Direct2D via Camadas do SDK.
	options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

	// Inicialize a Fábrica Direct2D.
	DX::ThrowIfFailed(
		D2D1CreateFactory(
			D2D1_FACTORY_TYPE_SINGLE_THREADED,
			__uuidof(ID2D1Factory3),
			&options,
			&m_d2dFactory
			)
		);

	// Inicialize a Fábrica DirectWrite.
	DX::ThrowIfFailed(
		DWriteCreateFactory(
			DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(IDWriteFactory3),
			&m_dwriteFactory
			)
		);

	// Inicialize a Fábrica do Windows Imaging Component (WIC).
	DX::ThrowIfFailed(
		CoCreateInstance(
			CLSID_WICImagingFactory2,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&m_wicFactory)
			)
		);
}

// Configura o dispositivo Direct3D e armazena identificadores e o contexto de dispositivo nele.
void DX::DeviceResources::CreateDeviceResources() 
{
	// Este sinalizador adiciona suporte a superfícies com uma ordenação de canal de cor diferente
	// do padrão da API. Isso é necessário para compatibilidade com o Direct2D.
	UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#if defined(_DEBUG)
	if (DX::SdkLayersAvailable())
	{
		// Se o projeto estiver em uma compilação de depuração, habilite a depuração via Camadas do SDK com este sinalizador.
		creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
	}
#endif

	// Esta matriz define o conjunto de níveis de recurso de hardware do DirectX ao qual este aplicativo dará suporte.
	// Observe que a ordem deve ser preservada.
	// Não se esqueça de declarar o nível mínimo de recurso necessário de seu aplicativo na
	// descrição. Supõe-se que todos os aplicativos dão suporte a 9.1, salvo indicação em contrário.
	D3D_FEATURE_LEVEL featureLevels[] = 
	{
		D3D_FEATURE_LEVEL_12_1,
		D3D_FEATURE_LEVEL_12_0,
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
		D3D_FEATURE_LEVEL_9_2,
		D3D_FEATURE_LEVEL_9_1
	};

	// Crie o objeto do dispositivo de API do Direct3D 11 e um contexto correspondente.
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;

	HRESULT hr = D3D11CreateDevice(
		nullptr,					// Especifique nullptr para usar o adaptador padrão.
		D3D_DRIVER_TYPE_HARDWARE,	// Crie um dispositivo usando um driver de gráficos de hardware.
		0,							// Deve ser 0, a menos que o driver seja D3D_DRIVER_TYPE_SOFTWARE.
		creationFlags,				// Defina sinalizadores de compatibilidade de depuração e do Direct2D.
		featureLevels,				// Lista de níveis de recursos aos quais este aplicativo pode dar suporte.
		ARRAYSIZE(featureLevels),	// O tamanho da lista acima.
		D3D11_SDK_VERSION,			// Defina sempre como D3D11_SDK_VERSION para aplicativos da Microsoft Store.
		&device,					// Retorna o dispositivo Direct3D criado.
		&m_d3dFeatureLevel,			// Retorna o nível de recurso do dispositivo criado.
		&context					// Retorna o contexto imediato do dispositivo.
		);

	if (FAILED(hr))
	{
		// Se a inicialização falhar, retorne para o dispositivo WARP.
		// Para obter mais informações sobre o WARP, consulte: 
		// https://go.microsoft.com/fwlink/?LinkId=286690
		DX::ThrowIfFailed(
			D3D11CreateDevice(
				nullptr,
				D3D_DRIVER_TYPE_WARP, // Crie um dispositivo WARP em vez de um dispositivo de hardware.
				0,
				creationFlags,
				featureLevels,
				ARRAYSIZE(featureLevels),
				D3D11_SDK_VERSION,
				&device,
				&m_d3dFeatureLevel,
				&context
				)
			);
	}

	// Armazene ponteiros para o dispositivo da API do Direct3D 11.3 e o contexto imediato.
	DX::ThrowIfFailed(
		device.As(&m_d3dDevice)
		);

	DX::ThrowIfFailed(
		context.As(&m_d3dContext)
		);

	// Crie o objeto do dispositivo Direct2D e um contexto correspondente.
	ComPtr<IDXGIDevice3> dxgiDevice;
	DX::ThrowIfFailed(
		m_d3dDevice.As(&dxgiDevice)
		);

	DX::ThrowIfFailed(
		m_d2dFactory->CreateDevice(dxgiDevice.Get(), &m_d2dDevice)
		);

	DX::ThrowIfFailed(
		m_d2dDevice->CreateDeviceContext(
			D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
			&m_d2dContext
			)
		);
}

// Esses recursos devem ser recriados sempre que o tamanho da janela for alterado.
void DX::DeviceResources::CreateWindowSizeDependentResources() 
{
	// Apague o contexto específico do tamanho da janela anterior.
	ID3D11RenderTargetView* nullViews[] = {nullptr};
	m_d3dContext->OMSetRenderTargets(ARRAYSIZE(nullViews), nullViews, nullptr);
	m_d3dRenderTargetView = nullptr;
	m_d2dContext->SetTarget(nullptr);
	m_d2dTargetBitmap = nullptr;
	m_d3dDepthStencilView = nullptr;
	m_d3dContext->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);

	UpdateRenderTargetSize();

	// A largura e a altura da cadeia de troca deve basear-se na
	// largura e na altura da orientação nativa da janela. Se a janela não estiver na
	// orientação nativa, as dimensões deverão ser invertidas.
	DXGI_MODE_ROTATION displayRotation = ComputeDisplayRotation();

	bool swapDimensions = displayRotation == DXGI_MODE_ROTATION_ROTATE90 || displayRotation == DXGI_MODE_ROTATION_ROTATE270;
	m_d3dRenderTargetSize.Width = swapDimensions ? m_outputSize.Height : m_outputSize.Width;
	m_d3dRenderTargetSize.Height = swapDimensions ? m_outputSize.Width : m_outputSize.Height;

	if (m_swapChain != nullptr)
	{
		// Se a cadeia de troca já existir, redimensione-a.
		HRESULT hr = m_swapChain->ResizeBuffers(
			2, // Cadeia de troca com buffer duplo.
			lround(m_d3dRenderTargetSize.Width),
			lround(m_d3dRenderTargetSize.Height),
			DXGI_FORMAT_B8G8R8A8_UNORM,
			0
			);

		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			// Se o dispositivo for removido por qualquer motivo, um novo dispositivo e cadeia de troca precisarão ser criados.
			HandleDeviceLost();

			// Tudo está configurado agora. Não continue a execução deste método. HandleDeviceLost reinserirá este método 
			// e configurará corretamente o novo dispositivo.
			return;
		}
		else
		{
			DX::ThrowIfFailed(hr);
		}
	}
	else
	{
		// Caso contrário, crie um novo usando o mesmo adaptador que o dispositivo Direct3D existente.
		DXGI_SCALING scaling = DisplayMetrics::SupportHighResolutions ? DXGI_SCALING_NONE : DXGI_SCALING_STRETCH;
		DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {0};

		swapChainDesc.Width = lround(m_d3dRenderTargetSize.Width);		// Corresponda ao tamanho da janela.
		swapChainDesc.Height = lround(m_d3dRenderTargetSize.Height);
		swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;				// Este é o formato de cadeia de troca mais comum.
		swapChainDesc.Stereo = false;
		swapChainDesc.SampleDesc.Count = 1;								// Não use diversas amostras.
		swapChainDesc.SampleDesc.Quality = 0;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.BufferCount = 2;									// Use o buffer duplo para minimizar a latência.
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;	// Todos os aplicativos da Microsoft Store devem usar SwapEffects _FLIP_.
		swapChainDesc.Flags = 0;
		swapChainDesc.Scaling = scaling;
		swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

		// Esta sequência obtém a fábrica DXGI usada para criar o dispositivo Direct3D acima.
		ComPtr<IDXGIDevice3> dxgiDevice;
		DX::ThrowIfFailed(
			m_d3dDevice.As(&dxgiDevice)
			);

		ComPtr<IDXGIAdapter> dxgiAdapter;
		DX::ThrowIfFailed(
			dxgiDevice->GetAdapter(&dxgiAdapter)
			);

		ComPtr<IDXGIFactory4> dxgiFactory;
		DX::ThrowIfFailed(
			dxgiAdapter->GetParent(IID_PPV_ARGS(&dxgiFactory))
			);

		// Ao usar a interoperabilidade XAML, a cadeia de troca deve ser criada para composição.
		ComPtr<IDXGISwapChain1> swapChain;
		DX::ThrowIfFailed(
			dxgiFactory->CreateSwapChainForComposition(
				m_d3dDevice.Get(),
				&swapChainDesc,
				nullptr,
				&swapChain
				)
			);

		DX::ThrowIfFailed(
			swapChain.As(&m_swapChain)
			);

		// Associe a cadeia de troca ao SwapChainPanel
		// Alterações na interface do usuário precisarão ser enviadas de volta ao thread da IU
		auto attachSwapChain = [this]()
		{
			// Obtenha a interface nativa de suporte para o SwapChainPanel
			ComPtr<ISwapChainPanelNative> panelNative;
			DX::ThrowIfFailed(
				reinterpret_cast<IUnknown*>(m_swapChainPanel)->QueryInterface(IID_PPV_ARGS(&panelNative))
				);

			DX::ThrowIfFailed(
				panelNative->SetSwapChain(m_swapChain.Get())
				);
		};
		if (m_swapChainPanel->Dispatcher->HasThreadAccess)
			attachSwapChain();
		else
			m_swapChainPanel->Dispatcher->RunAsync(
				CoreDispatcherPriority::High,
				ref new DispatchedHandler(attachSwapChain));

		// Assegure que o DXGI não coloque na fila mais de um quadro por vez. Isso reduz a latência e
		// garante que o aplicativo renderizará somente após cada VSync, minimizando o consumo de energia.
		DX::ThrowIfFailed(
			dxgiDevice->SetMaximumFrameLatency(1)
			);
	}

	// Defina a orientação correta da cadeia de troca e gere as
	// transformações de matrizes 2D e 3D para renderização da cadeia de troca girada.
	// Observe que o ângulo de rotação das transformações 2D e 3D são diferentes.
	// Isso ocorre devido à diferença nos espaços coordenados. Além disso,
	// a matriz 3D é especificada explicitamente para evitar erros de arredondamento.

	switch (displayRotation)
	{
	case DXGI_MODE_ROTATION_IDENTITY:
		m_orientationTransform2D = Matrix3x2F::Identity();
		m_orientationTransform3D = ScreenRotation::Rotation0;
		break;

	case DXGI_MODE_ROTATION_ROTATE90:
		m_orientationTransform2D = 
			Matrix3x2F::Rotation(90.0f) *
			Matrix3x2F::Translation(m_logicalSize.Height, 0.0f);
		m_orientationTransform3D = ScreenRotation::Rotation270;
		break;

	case DXGI_MODE_ROTATION_ROTATE180:
		m_orientationTransform2D = 
			Matrix3x2F::Rotation(180.0f) *
			Matrix3x2F::Translation(m_logicalSize.Width, m_logicalSize.Height);
		m_orientationTransform3D = ScreenRotation::Rotation180;
		break;

	case DXGI_MODE_ROTATION_ROTATE270:
		m_orientationTransform2D = 
			Matrix3x2F::Rotation(270.0f) *
			Matrix3x2F::Translation(0.0f, m_logicalSize.Width);
		m_orientationTransform3D = ScreenRotation::Rotation90;
		break;

	default:
		throw ref new FailureException();
	}

	DX::ThrowIfFailed(
		m_swapChain->SetRotation(displayRotation)
		);

	// Configure a escala inversa na cadeia de troca
	DXGI_MATRIX_3X2_F inverseScale = { 0 };
	inverseScale._11 = 1.0f / m_effectiveCompositionScaleX;
	inverseScale._22 = 1.0f / m_effectiveCompositionScaleY;
	ComPtr<IDXGISwapChain2> spSwapChain2;
	DX::ThrowIfFailed(
		m_swapChain.As<IDXGISwapChain2>(&spSwapChain2)
		);

	DX::ThrowIfFailed(
		spSwapChain2->SetMatrixTransform(&inverseScale)
		);

	// Crie uma exibição de destino de renderização do buffer de fundo da cadeia de troca.
	ComPtr<ID3D11Texture2D1> backBuffer;
	DX::ThrowIfFailed(
		m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))
		);

	DX::ThrowIfFailed(
		m_d3dDevice->CreateRenderTargetView1(
			backBuffer.Get(),
			nullptr,
			&m_d3dRenderTargetView
			)
		);

	// Crie uma exibição do estêncil de profundidade para usar com a renderização 3D, se necessário.
	CD3D11_TEXTURE2D_DESC1 depthStencilDesc(
		DXGI_FORMAT_D24_UNORM_S8_UINT, 
		lround(m_d3dRenderTargetSize.Width),
		lround(m_d3dRenderTargetSize.Height),
		1, // Esta exibição do estêncil de profundidade tem somente uma textura.
		1, // Use um único nível de mipmap.
		D3D11_BIND_DEPTH_STENCIL
		);

	ComPtr<ID3D11Texture2D1> depthStencil;
	DX::ThrowIfFailed(
		m_d3dDevice->CreateTexture2D1(
			&depthStencilDesc,
			nullptr,
			&depthStencil
			)
		);

	CD3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc(D3D11_DSV_DIMENSION_TEXTURE2D);
	DX::ThrowIfFailed(
		m_d3dDevice->CreateDepthStencilView(
			depthStencil.Get(),
			&depthStencilViewDesc,
			&m_d3dDepthStencilView
			)
		);

	// Defina o visor de renderização 3D como destino de toda a janela.
	m_screenViewport = CD3D11_VIEWPORT(
		0.0f,
		0.0f,
		m_d3dRenderTargetSize.Width,
		m_d3dRenderTargetSize.Height
		);

	m_d3dContext->RSSetViewports(1, &m_screenViewport);

	// Crie um bitmap de destino do Direct2D associado ao
	// buffer de fundo da cadeia de troca e defina-o como o destino atual.
	D2D1_BITMAP_PROPERTIES1 bitmapProperties = 
		D2D1::BitmapProperties1(
			D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
			m_dpi,
			m_dpi
			);

	ComPtr<IDXGISurface2> dxgiBackBuffer;
	DX::ThrowIfFailed(
		m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer))
		);

	DX::ThrowIfFailed(
		m_d2dContext->CreateBitmapFromDxgiSurface(
			dxgiBackBuffer.Get(),
			&bitmapProperties,
			&m_d2dTargetBitmap
			)
		);

	m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
	m_d2dContext->SetDpi(m_effectiveDpi, m_effectiveDpi);

	// A suavização do texto em escala de cinza é recomendada para todos os aplicativos da Microsoft Store.
	m_d2dContext->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

// Determine as dimensões do destino de renderização e se ele será reduzido verticalmente.
void DX::DeviceResources::UpdateRenderTargetSize()
{
	m_effectiveDpi = m_dpi;
	m_effectiveCompositionScaleX = m_compositionScaleX;
	m_effectiveCompositionScaleY = m_compositionScaleY;

	// Para aumentar a vida útil da bateria em dispositivos de alta resolução, renderize para um destino de renderização menor
	// e permita à GPU escalar a saída quando ela é apresentada.
	if (!DisplayMetrics::SupportHighResolutions && m_dpi > DisplayMetrics::DpiThreshold)
	{
		float width = DX::ConvertDipsToPixels(m_logicalSize.Width, m_dpi);
		float height = DX::ConvertDipsToPixels(m_logicalSize.Height, m_dpi);

		// Quando o dispositivo estiver na orientação retrato, altura > largura. Compare a
		// dimensão maior com o limite de largura e a dimensão menor
		// com o limite de altura.
		if ((std::max)(width, height) > DisplayMetrics::WidthThreshold &&
			(std::min)(width, height) > DisplayMetrics::HeightThreshold)
		{
			// Para escalar o aplicativo, alteramos o DPI efetivo. O tamanho lógico não é alterado.
			m_effectiveDpi /= 2.0f;
			m_effectiveCompositionScaleX /= 2.0f;
			m_effectiveCompositionScaleY /= 2.0f;
		}
	}

	// Calcule o tamanho de destino de processamento necessário em pixels.
	m_outputSize.Width = DX::ConvertDipsToPixels(m_logicalSize.Width, m_effectiveDpi);
	m_outputSize.Height = DX::ConvertDipsToPixels(m_logicalSize.Height, m_effectiveDpi);

	// Evite que conteúdo do DirectX com tamanho zero seja criado.
	m_outputSize.Width = (std::max)(m_outputSize.Width, 1.0f);
	m_outputSize.Height = (std::max)(m_outputSize.Height, 1.0f);
}

// Este método é chamado quando o controle XAML é criado (ou recriado).
void DX::DeviceResources::SetSwapChainPanel(SwapChainPanel^ panel)
{
	DisplayInformation^ currentDisplayInformation = DisplayInformation::GetForCurrentView();

	m_swapChainPanel = panel;
	m_logicalSize = Windows::Foundation::Size(static_cast<float>(panel->ActualWidth), static_cast<float>(panel->ActualHeight));
	m_nativeOrientation = currentDisplayInformation->NativeOrientation;
	m_currentOrientation = currentDisplayInformation->CurrentOrientation;
	m_compositionScaleX = panel->CompositionScaleX;
	m_compositionScaleY = panel->CompositionScaleY;
	m_dpi = currentDisplayInformation->LogicalDpi;
	m_d2dContext->SetDpi(m_dpi, m_dpi);

	CreateWindowSizeDependentResources();
}

void DX::DeviceResources::ReleaseSizeDependentResourcesForExternalRenderer()
{
	// The embedded Cemu renderer owns presentation and ResizeBuffers after the
	// swap chain has been attached to the SwapChainPanel. Keeping any of the
	// template render-target objects alive makes IDXGISwapChain::ResizeBuffers
	// fail with DXGI_ERROR_INVALID_CALL when the XAML layout changes.
	ID3D11RenderTargetView* nullViews[] = { nullptr };
	m_d3dContext->OMSetRenderTargets(ARRAYSIZE(nullViews), nullViews, nullptr);
	m_d2dContext->SetTarget(nullptr);
	m_d3dRenderTargetView = nullptr;
	m_d2dTargetBitmap = nullptr;
	m_d3dDepthStencilView = nullptr;
	m_d3dContext->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
}

// Este método é chamado no manipulador de eventos para o evento SizeChanged.
void DX::DeviceResources::SetLogicalSize(Windows::Foundation::Size logicalSize)
{
	if (m_logicalSize != logicalSize)
	{
		m_logicalSize = logicalSize;
		CreateWindowSizeDependentResources();
	}
}

// Este método é chamado no manipulador de eventos para o evento DpiChanged.
void DX::DeviceResources::SetDpi(float dpi)
{
	if (dpi != m_dpi)
	{
		m_dpi = dpi;
		m_d2dContext->SetDpi(m_dpi, m_dpi);
		CreateWindowSizeDependentResources();
	}
}

// Este método é chamado no manipulador de eventos para o evento OrientationChanged.
void DX::DeviceResources::SetCurrentOrientation(DisplayOrientations currentOrientation)
{
	if (m_currentOrientation != currentOrientation)
	{
		m_currentOrientation = currentOrientation;
		CreateWindowSizeDependentResources();
	}
}

// Este método é chamado no manipulador de eventos para o evento CompositionScaleChanged.
void DX::DeviceResources::SetCompositionScale(float compositionScaleX, float compositionScaleY)
{
	if (m_compositionScaleX != compositionScaleX ||
		m_compositionScaleY != compositionScaleY)
	{
		m_compositionScaleX = compositionScaleX;
		m_compositionScaleY = compositionScaleY;
		CreateWindowSizeDependentResources();
	}
}

// Este método é chamado no manipulador de eventos para o evento DisplayContentsInvalidated.
void DX::DeviceResources::ValidateDevice()
{
	// O Dispositivo D3D não será mais válido se o adaptador padrão for alterado após o dispositivo
	// ser criado ou se o dispositivo foi removido.

	// Em primeiro lugar, obtenha as informações para o adaptador padrão a partir de quando o dispositivo foi criado.

	ComPtr<IDXGIDevice3> dxgiDevice;
	DX::ThrowIfFailed(m_d3dDevice.As(&dxgiDevice));

	ComPtr<IDXGIAdapter> deviceAdapter;
	DX::ThrowIfFailed(dxgiDevice->GetAdapter(&deviceAdapter));

	ComPtr<IDXGIFactory2> deviceFactory;
	DX::ThrowIfFailed(deviceAdapter->GetParent(IID_PPV_ARGS(&deviceFactory)));

	ComPtr<IDXGIAdapter1> previousDefaultAdapter;
	DX::ThrowIfFailed(deviceFactory->EnumAdapters1(0, &previousDefaultAdapter));

	DXGI_ADAPTER_DESC1 previousDesc;
	DX::ThrowIfFailed(previousDefaultAdapter->GetDesc1(&previousDesc));

	// Em seguida, obtenha as informações para o adaptador padrão atual.

	ComPtr<IDXGIFactory4> currentFactory;
	DX::ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&currentFactory)));

	ComPtr<IDXGIAdapter1> currentDefaultAdapter;
	DX::ThrowIfFailed(currentFactory->EnumAdapters1(0, &currentDefaultAdapter));

	DXGI_ADAPTER_DESC1 currentDesc;
	DX::ThrowIfFailed(currentDefaultAdapter->GetDesc1(&currentDesc));

	// Se os LUIDs do adaptador não corresponderem ou se os relatórios do dispositivo tiverem sido removidos,
	// um novo dispositivo D3D deverá ser criado.

	if (previousDesc.AdapterLuid.LowPart != currentDesc.AdapterLuid.LowPart ||
		previousDesc.AdapterLuid.HighPart != currentDesc.AdapterLuid.HighPart ||
		FAILED(m_d3dDevice->GetDeviceRemovedReason()))
	{
		// Libere referências aos recursos relacionados ao dispositivo antigo.
		dxgiDevice = nullptr;
		deviceAdapter = nullptr;
		deviceFactory = nullptr;
		previousDefaultAdapter = nullptr;

		// Crie um novo dispositivo e cadeia de troca.
		HandleDeviceLost();
	}
}

// Recrie todos os recursos de dispositivo e configure-os de volta ao estado atual.
void DX::DeviceResources::HandleDeviceLost()
{
	m_swapChain = nullptr;

	if (m_deviceNotify != nullptr)
	{
		m_deviceNotify->OnDeviceLost();
	}

	CreateDeviceResources();
	m_d2dContext->SetDpi(m_dpi, m_dpi);
	CreateWindowSizeDependentResources();

	if (m_deviceNotify != nullptr)
	{
		m_deviceNotify->OnDeviceRestored();
	}
}

// Registre nossa DeviceNotify para ser informado da perda e da criação de dispositivos.
void DX::DeviceResources::RegisterDeviceNotify(DX::IDeviceNotify* deviceNotify)
{
	m_deviceNotify = deviceNotify;
}

// Chame este método quando o aplicativo for suspenso. Ele fornece uma dica para o driver de que o aplicativo 
// está entrando em estado ocioso e os buffers temporários podem ser recuperados para uso por outros aplicativos.
void DX::DeviceResources::Trim()
{
	ComPtr<IDXGIDevice3> dxgiDevice;
	m_d3dDevice.As(&dxgiDevice);

	dxgiDevice->Trim();
}

// Apresente o conteúdo da cadeia de troca na tela.
void DX::DeviceResources::Present() 
{
	// O primeiro argumento instrui o DXGI para bloquear até o VSync, colocando o aplicativo
	// em suspensão até o próximo VSync. Isso garante que não desperdiçaremos ciclos renderizando
	// quadros que nunca serão exibidos na tela.
	DXGI_PRESENT_PARAMETERS parameters = { 0 };
	HRESULT hr = m_swapChain->Present1(1, 0, &parameters);

	// Descarte o conteúdo do destino de renderização.
	// Esta será uma operação válida somente quando o conteúdo existente for totalmente
	// substituído. Se retângulos dirty ou scroll forem usados, você deverá modificar essa chamada.
	m_d3dContext->DiscardView1(m_d3dRenderTargetView.Get(), nullptr, 0);

	// Descarte o conteúdo do estêncil de profundidade.
	m_d3dContext->DiscardView1(m_d3dDepthStencilView.Get(), nullptr, 0);

	// Se o dispositivo foi removido por uma desconexão ou uma atualização de driver, 
	// será necessário recriar todos os recursos do dispositivo.
	if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
	{
		HandleDeviceLost();
	}
	else
	{
		DX::ThrowIfFailed(hr);
	}
}

// Este método determina a rotação entre a orientação nativa do dispositivo de vídeo e o
// orientação de exibição atual.
DXGI_MODE_ROTATION DX::DeviceResources::ComputeDisplayRotation()
{
	DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;

	// Observação: NativeOrientation somente poderá ser Paisagem ou Retrato, mesmo se
	// a enumeração DisplayOrientations tiver outros valores.
	switch (m_nativeOrientation)
	{
	case DisplayOrientations::Landscape:
		switch (m_currentOrientation)
		{
		case DisplayOrientations::Landscape:
			rotation = DXGI_MODE_ROTATION_IDENTITY;
			break;

		case DisplayOrientations::Portrait:
			rotation = DXGI_MODE_ROTATION_ROTATE270;
			break;

		case DisplayOrientations::LandscapeFlipped:
			rotation = DXGI_MODE_ROTATION_ROTATE180;
			break;

		case DisplayOrientations::PortraitFlipped:
			rotation = DXGI_MODE_ROTATION_ROTATE90;
			break;
		}
		break;

	case DisplayOrientations::Portrait:
		switch (m_currentOrientation)
		{
		case DisplayOrientations::Landscape:
			rotation = DXGI_MODE_ROTATION_ROTATE90;
			break;

		case DisplayOrientations::Portrait:
			rotation = DXGI_MODE_ROTATION_IDENTITY;
			break;

		case DisplayOrientations::LandscapeFlipped:
			rotation = DXGI_MODE_ROTATION_ROTATE270;
			break;

		case DisplayOrientations::PortraitFlipped:
			rotation = DXGI_MODE_ROTATION_ROTATE180;
			break;
		}
		break;
	}
	return rotation;
}
