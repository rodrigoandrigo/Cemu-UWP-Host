#pragma once

#include <ppltasks.h>	// Para create_task

namespace DX
{
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr))
		{
			// Defina um ponto de interrupção nesta linha para detectar erros da API do Win32.
			throw Platform::Exception::CreateException(hr);
		}
	}

	// Função que lê de um arquivo binário assincronamente.
	inline Concurrency::task<std::vector<byte>> ReadDataAsync(const std::wstring& filename)
	{
		using namespace Windows::Storage;
		using namespace Concurrency;

		auto folder = Windows::ApplicationModel::Package::Current->InstalledLocation;

		return create_task(folder->GetFileAsync(Platform::StringReference(filename.c_str()))).then([] (StorageFile^ file) 
		{
			return FileIO::ReadBufferAsync(file);
		}).then([] (Streams::IBuffer^ fileBuffer) -> std::vector<byte> 
		{
			std::vector<byte> returnBuffer;
			returnBuffer.resize(fileBuffer->Length);
			Streams::DataReader::FromBuffer(fileBuffer)->ReadBytes(Platform::ArrayReference<byte>(returnBuffer.data(), fileBuffer->Length));
			return returnBuffer;
		});
	}

	// Converte um tamanho em DPI (pixel independente de dispositivo) em um tamanho em pixels físicos.
	inline float ConvertDipsToPixels(float dips, float dpi)
	{
		static const float dipsPerInch = 96.0f;
		return floorf(dips * dpi / dipsPerInch + 0.5f); // Arredonde para o inteiro mais próximo.
	}

#if defined(_DEBUG)
	// Verifique o suporte de Camadas SDK.
	inline bool SdkLayersAvailable()
	{
		HRESULT hr = D3D11CreateDevice(
			nullptr,
			D3D_DRIVER_TYPE_NULL,       // Não é necessário criar um dispositivo de hardware real.
			0,
			D3D11_CREATE_DEVICE_DEBUG,  // Verifique as camadas do SDK.
			nullptr,                    // Qualquer nível de recurso servirá.
			0,
			D3D11_SDK_VERSION,          // Defina sempre como D3D11_SDK_VERSION para aplicativos da Microsoft Store.
			nullptr,                    // Não é necessário manter a referência do dispositivo D3D.
			nullptr,                    // Não é necessário saber o nível do recurso.
			nullptr                     // Não é necessário manter a referência do contexto do dispositivo D3D.
			);

		return SUCCEEDED(hr);
	}
#endif
}
