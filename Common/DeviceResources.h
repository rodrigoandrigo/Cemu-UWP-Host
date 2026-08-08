#pragma once

namespace DX
{
	// Fornece uma interface para um aplicativo que possua DeviceResources a serem notificados da perda ou da criação do dispositivo.
	interface IDeviceNotify
	{
		virtual void OnDeviceLost() = 0;
		virtual void OnDeviceRestored() = 0;
	};

	// Controla todos os recursos do dispositivo DirectX.
	class DeviceResources
	{
	public:
		// externalRenderer skips template D2D/DWrite/WIC and depth resources.
		// Use it when an embedded renderer exclusively owns the swap chain.
		explicit DeviceResources(bool externalRenderer = false);
		void SetSwapChainPanel(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);
		void ReleaseSizeDependentResourcesForExternalRenderer();
		void SetLogicalSize(Windows::Foundation::Size logicalSize);
		void SetCurrentOrientation(Windows::Graphics::Display::DisplayOrientations currentOrientation);
		void SetDpi(float dpi);
		void SetCompositionScale(float compositionScaleX, float compositionScaleY);
		void ValidateDevice();
		void HandleDeviceLost();
		void RegisterDeviceNotify(IDeviceNotify* deviceNotify);
		void Trim();
		void Present();

		// O tamanho do destino de renderização, em pixels.
		Windows::Foundation::Size	GetOutputSize() const					{ return m_outputSize; }

		// O tamanho do destino de renderização, em DIPs.
		Windows::Foundation::Size	GetLogicalSize() const					{ return m_logicalSize; }
		float						GetDpi() const							{ return m_effectiveDpi; }

		// Acessadores D3D.
		ID3D11Device3*				GetD3DDevice() const					{ return m_d3dDevice.Get(); }
		ID3D11DeviceContext3*		GetD3DDeviceContext() const				{ return m_d3dContext.Get(); }
		IDXGISwapChain3*			GetSwapChain() const					{ return m_swapChain.Get(); }
		D3D_FEATURE_LEVEL			GetDeviceFeatureLevel() const			{ return m_d3dFeatureLevel; }
		ID3D11RenderTargetView1*	GetBackBufferRenderTargetView() const	{ return m_d3dRenderTargetView.Get(); }
		ID3D11DepthStencilView*		GetDepthStencilView() const				{ return m_d3dDepthStencilView.Get(); }
		D3D11_VIEWPORT				GetScreenViewport() const				{ return m_screenViewport; }
		DirectX::XMFLOAT4X4			GetOrientationTransform3D() const		{ return m_orientationTransform3D; }

		// Acessadores D2D.
		ID2D1Factory3*				GetD2DFactory() const					{ return m_d2dFactory.Get(); }
		ID2D1Device2*				GetD2DDevice() const					{ return m_d2dDevice.Get(); }
		ID2D1DeviceContext2*		GetD2DDeviceContext() const				{ return m_d2dContext.Get(); }
		ID2D1Bitmap1*				GetD2DTargetBitmap() const				{ return m_d2dTargetBitmap.Get(); }
		IDWriteFactory3*			GetDWriteFactory() const				{ return m_dwriteFactory.Get(); }
		IWICImagingFactory2*		GetWicImagingFactory() const			{ return m_wicFactory.Get(); }
		D2D1::Matrix3x2F			GetOrientationTransform2D() const		{ return m_orientationTransform2D; }

	private:
		void CreateDeviceIndependentResources();
		void CreateDeviceResources();
		void CreateWindowSizeDependentResources();
		void UpdateRenderTargetSize();
		DXGI_MODE_ROTATION ComputeDisplayRotation();

		// Objetos Direct3D.
		Microsoft::WRL::ComPtr<ID3D11Device3>			m_d3dDevice;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext3>	m_d3dContext;
		Microsoft::WRL::ComPtr<IDXGISwapChain3>			m_swapChain;

		// Objetos de renderização Direct3D. Necessário para 3D.
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView1>	m_d3dRenderTargetView;
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView>	m_d3dDepthStencilView;
		D3D11_VIEWPORT									m_screenViewport;

		// Componentes de desenho Direct2D.
		Microsoft::WRL::ComPtr<ID2D1Factory3>		m_d2dFactory;
		Microsoft::WRL::ComPtr<ID2D1Device2>		m_d2dDevice;
		Microsoft::WRL::ComPtr<ID2D1DeviceContext2>	m_d2dContext;
		Microsoft::WRL::ComPtr<ID2D1Bitmap1>		m_d2dTargetBitmap;

		// Componentes de desenho DirectWrite.
		Microsoft::WRL::ComPtr<IDWriteFactory3>		m_dwriteFactory;
		Microsoft::WRL::ComPtr<IWICImagingFactory2>	m_wicFactory;

		// Referência armazenada em cache para o painel XAML.
		Windows::UI::Xaml::Controls::SwapChainPanel^	m_swapChainPanel;

		// Propriedades do dispositivo armazenadas em cache.
		D3D_FEATURE_LEVEL								m_d3dFeatureLevel;
		Windows::Foundation::Size						m_d3dRenderTargetSize;
		Windows::Foundation::Size						m_outputSize;
		Windows::Foundation::Size						m_logicalSize;
		Windows::Graphics::Display::DisplayOrientations	m_nativeOrientation;
		Windows::Graphics::Display::DisplayOrientations	m_currentOrientation;
		float											m_dpi;
		float											m_compositionScaleX;
		float											m_compositionScaleY;

		// Variáveis que levam em conta se o aplicativo dá suporte a telas de alta resolução ou não.
		float											m_effectiveDpi;
		float											m_effectiveCompositionScaleX;
		float											m_effectiveCompositionScaleY;

		// Transformações usadas para orientação da exibição.
		D2D1::Matrix3x2F	m_orientationTransform2D;
		DirectX::XMFLOAT4X4	m_orientationTransform3D;

		// O IDeviceNotify pode ser mantido diretamente, pois tem os DeviceResources.
		IDeviceNotify* m_deviceNotify;
		bool m_externalRenderer;
	};
}
