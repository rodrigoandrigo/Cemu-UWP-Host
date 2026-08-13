//
// App.xaml.h
// Declaração da classe App.
//

#pragma once

#include "App.g.h"
#include "DirectXPage.xaml.h"

namespace Cemu_UWP_Host
{
		/// <summary>
	/// Fornece o comportamento específico do aplicativo para complementar a classe Application padrão.
	/// </summary>
	ref class App sealed
	{
	public:
		App();
		virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;

	private:
		void EnterFullScreen();
		void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ e);
		void OnResuming(Platform::Object ^sender, Platform::Object ^args);
		void OnNavigationFailed(Platform::Object ^sender, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs ^e);
		DirectXPage^ m_directXPage;
	};
}
