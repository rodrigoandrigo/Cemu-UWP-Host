#include "pch.h"
#include "Cemu_UWP_HostMain.h"

#include <algorithm>
#include <utility>

using namespace Cemu_UWP_Host;
using namespace concurrency;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;

namespace
{
std::string ToUtf8(Platform::String^ value)
{
	if (!value) return {};
	const auto length = WideCharToMultiByte(CP_UTF8, 0, value->Data(), -1, nullptr, 0, nullptr, nullptr);
	std::string result(length > 0 ? length : 0, '\0');
	if (length > 1) WideCharToMultiByte(CP_UTF8, 0, value->Data(), -1, &result[0], length, nullptr, nullptr);
	if (!result.empty()) result.pop_back();
	return result;
}

struct BrokeredFile
{
	StorageFile^ file;
};

struct BrokeredStream
{
	IRandomAccessStream^ stream;
	DataReader^ reader;
};

CemuEmbedResult EnumerateFolder(StorageFolder^ folder, const std::string& relativePath,
	CemuEmbedBrokeredEntryCallback entryCallback, void* entryCallbackUserData)
{
	constexpr unsigned int pageSize = 256;
	unsigned int startIndex = 0;
	for (;;)
	{
		const auto items = create_task(folder->GetItemsAsync(startIndex, pageSize)).get();
		for (const auto& item : items)
		{
			const auto name = ToUtf8(item->Name);
			const auto itemPath = relativePath.empty() ? name : relativePath + "/" + name;
			if (auto childFolder = dynamic_cast<StorageFolder^>(item))
			{
				if (entryCallback(entryCallbackUserData, itemPath.c_str(), CEMU_EMBED_BROKERED_DIRECTORY, 0, nullptr) != CEMU_EMBED_OK)
					return CEMU_EMBED_STORAGE_FAILED;
				const auto result = EnumerateFolder(childFolder, itemPath, entryCallback, entryCallbackUserData);
				if (result != CEMU_EMBED_OK) return result;
			}
			else if (auto file = dynamic_cast<StorageFile^>(item))
			{
				auto properties = create_task(file->GetBasicPropertiesAsync()).get();
				BrokeredFile brokeredFile{ file };
				const auto result = entryCallback(entryCallbackUserData, itemPath.c_str(), CEMU_EMBED_BROKERED_FILE, properties->Size, &brokeredFile);
				if (result != CEMU_EMBED_OK) return result;
			}
		}
		if (items->Size < pageSize) break;
		startIndex += items->Size;
	}
	return CEMU_EMBED_OK;
}
}

Cemu_UWP_HostMain::Cemu_UWP_HostMain(HWND window, const CemuEmbedD3D11Surface& surface, int width, int height, double dpiScale)
	: m_window(window), m_d3d11Surface(surface), m_width(width), m_height(height), m_dpiScale(dpiScale) {}

Cemu_UWP_HostMain::~Cemu_UWP_HostMain() { Stop(); }

bool Cemu_UWP_HostMain::Start()
{
	if (m_started) return true;
	const auto installed = Windows::ApplicationModel::Package::Current->InstalledLocation->Path;
	const auto local = Windows::Storage::ApplicationData::Current->LocalFolder->Path;
	const auto cache = Windows::Storage::ApplicationData::Current->LocalCacheFolder->Path;
	const auto executable = ToUtf8(installed);
	const auto userData = ToUtf8(local);
	const auto cachePath = ToUtf8(cache);
	CemuEmbedConfig config{ sizeof(config), CEMU_EMBED_ABI_VERSION, executable.c_str(), userData.c_str(), userData.c_str(), cachePath.c_str(), executable.c_str() };
	CemuEmbedCallbacks callbacks{ sizeof(callbacks), this, Log, Error, StateChanged };
	if (CemuEmbed_Create(&config, &callbacks, &m_instance) != CEMU_EMBED_OK)
		return false;
	CemuEmbedSurface surface{ sizeof(surface), m_window, &m_d3d11Surface, m_width, m_height, m_dpiScale };
	if (CemuEmbed_SetSurface(m_instance, &surface) != CEMU_EMBED_OK || CemuEmbed_InitializeAsync(m_instance) != CEMU_EMBED_OK)
	{
		CemuEmbed_Destroy(m_instance);
		m_instance = nullptr;
		return false;
	}
	m_started = true;
	return true;
}

bool Cemu_UWP_HostMain::LaunchGame(StorageFolder^ gameFolder)
{
	if (!m_instance || !gameFolder) return false;
	const CemuEmbedBrokeredStorage storage{
		sizeof(storage), CEMU_EMBED_BROKERED_STORAGE_VERSION, this,
		EnumerateBrokeredFolder, OpenBrokeredFile, ReadBrokeredStream,
		CloseBrokeredStream, BrokeredProgress
	};
	return CemuEmbed_LaunchGameFromBrokeredFolder(m_instance, reinterpret_cast<void*>(gameFolder), &storage) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::InstallTitle(StorageFolder^ titleFolder,
	CemuEmbedInstallType expectedType, uint64_t* installedBaseTitleId)
{
	if (!m_instance || !titleFolder) return false;
	const CemuEmbedBrokeredStorage storage{
		sizeof(storage), CEMU_EMBED_BROKERED_STORAGE_VERSION, this,
		EnumerateBrokeredFolder, OpenBrokeredFile, ReadBrokeredStream,
		CloseBrokeredStream, BrokeredProgress
	};
	return CemuEmbed_InstallTitleFromBrokeredFolder(m_instance,
		reinterpret_cast<void*>(titleFolder), &storage, expectedType,
		installedBaseTitleId) == CEMU_EMBED_OK;
}

std::vector<InstalledTitle> Cemu_UWP_HostMain::GetInstalledTitles()
{
	std::vector<InstalledTitle> titles;
	if (m_instance)
		CemuEmbed_EnumerateInstalledTitles(m_instance, InstalledTitleFound, &titles);
	return titles;
}

bool Cemu_UWP_HostMain::GetActiveAccount(ActiveAccount& account)
{
	if (!m_instance) return false;
	CemuEmbedActiveAccount embedded{};
	embedded.struct_size = sizeof(embedded);
	embedded.abi_version = CEMU_EMBED_ACCOUNT_VERSION;
	if (CemuEmbed_GetActiveAccount(m_instance, &embedded) != CEMU_EMBED_OK)
		return false;
	account.persistentId = embedded.persistent_id;
	account.onlineEnabled = embedded.online_enabled != 0;
	account.miiName = embedded.mii_name_utf8;
	account.accountId = embedded.account_id_utf8;
	return true;
}

bool Cemu_UWP_HostMain::LaunchInstalledTitle(uint64_t baseTitleId)
{
	return m_instance &&
		CemuEmbed_LaunchInstalledTitle(m_instance, baseTitleId) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::InstallGraphicPacks(StorageFolder^ graphicPacksFolder,
	uint32_t* importedPackCount)
{
	if (!m_instance || !graphicPacksFolder) return false;
	const CemuEmbedBrokeredStorage storage{
		sizeof(storage), CEMU_EMBED_BROKERED_STORAGE_VERSION, this,
		EnumerateBrokeredFolder, OpenBrokeredFile, ReadBrokeredStream,
		CloseBrokeredStream, BrokeredProgress
	};
	return CemuEmbed_InstallGraphicPacksFromBrokeredFolder(m_instance,
		reinterpret_cast<void*>(graphicPacksFolder), &storage,
		importedPackCount) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::ApplySafeGraphicPackPolicyForTitle(uint64_t baseTitleId,
	uint32_t* affectedPackCount)
{
	return m_instance &&
		CemuEmbed_ApplySafeGraphicPackPolicyForTitle(m_instance, baseTitleId,
			affectedPackCount) == CEMU_EMBED_OK;
}

bool Cemu_UWP_HostMain::EnsureDefaultGamepadProfile()
{
	if (!m_instance) return false;
	int32_t ready{};
	return CemuEmbed_EnsureDefaultGamepadProfile(m_instance, &ready) ==
		CEMU_EMBED_OK && ready != 0;
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::InstalledTitleFound(
	void* userData, const CemuEmbedInstalledTitle* title)
{
	if (!userData || !title ||
		title->struct_size < sizeof(CemuEmbedInstalledTitle) ||
		title->abi_version != CEMU_EMBED_LIBRARY_VERSION)
		return CEMU_EMBED_INVALID_ARGUMENT;
	auto& titles = *static_cast<std::vector<InstalledTitle>*>(userData);
	titles.push_back({
		title->title_id,
		title->base_version,
		title->effective_version,
		title->update_version,
		title->dlc_version,
		title->dlc_count,
		title->region,
		title->compatible_graphic_pack_count,
		title->enabled_graphic_pack_count,
		title->name_utf8 ? title->name_utf8 : "",
		title->region_utf8 ? title->region_utf8 : ""
	});
	return CEMU_EMBED_OK;
}

void Cemu_UWP_HostMain::SetDiagnosticCallback(std::function<void(const std::string&)> callback)
{
	m_diagnosticCallback = std::move(callback);
}

void Cemu_UWP_HostMain::SetStateCallback(std::function<void(CemuEmbedState)> callback)
{
	m_stateCallback = std::move(callback);
}

void Cemu_UWP_HostMain::SetProgressCallback(std::function<void(uint64_t, uint64_t, const std::string&)> callback)
{
	m_progressCallback = std::move(callback);
}

bool Cemu_UWP_HostMain::IsReady() const
{
	return m_ready.load(std::memory_order_acquire);
}

void Cemu_UWP_HostMain::ResizeSurface(int width, int height, double dpiScale)
{
	m_width = (std::max)(width, 1);
	m_height = (std::max)(height, 1);
	m_dpiScale = dpiScale > 0.0 ? dpiScale : 1.0;
	if (!m_instance) return;
	CemuEmbedSurface surface{ sizeof(surface), m_window, &m_d3d11Surface, m_width, m_height, m_dpiScale };
	CemuEmbed_SetSurface(m_instance, &surface);
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::EnumerateBrokeredFolder(void*, void* folderHandle,
	CemuEmbedBrokeredEntryCallback entryCallback, void* entryCallbackUserData)
{
	try
	{
		return EnumerateFolder(reinterpret_cast<StorageFolder^>(folderHandle), {}, entryCallback, entryCallbackUserData);
	}
	catch (...) { return CEMU_EMBED_STORAGE_FAILED; }
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::OpenBrokeredFile(void*, void* fileHandle, void** streamHandle)
{
	if (!fileHandle || !streamHandle) return CEMU_EMBED_INVALID_ARGUMENT;
	try
	{
		auto file = static_cast<BrokeredFile*>(fileHandle)->file;
		auto stream = create_task(file->OpenAsync(FileAccessMode::Read)).get();
		auto reader = ref new DataReader(stream);
		reader->InputStreamOptions = InputStreamOptions::Partial;
		*streamHandle = new BrokeredStream{ stream, reader };
		return CEMU_EMBED_OK;
	}
	catch (...) { return CEMU_EMBED_STORAGE_FAILED; }
}

CemuEmbedResult __cdecl Cemu_UWP_HostMain::ReadBrokeredStream(void*, void* streamHandle, uint64_t offset,
	uint8_t* buffer, uint32_t bufferSize, uint32_t* bytesRead)
{
	if (!streamHandle || !buffer || !bytesRead) return CEMU_EMBED_INVALID_ARGUMENT;
	try
	{
		auto brokeredStream = static_cast<BrokeredStream*>(streamHandle);
		auto stream = brokeredStream->stream;
		auto reader = brokeredStream->reader;
		if (stream->Position != offset) stream->Seek(offset);
		const auto count = create_task(reader->LoadAsync(bufferSize)).get();
		if (count)
			reader->ReadBytes(Platform::ArrayReference<unsigned char>(buffer, count));
		*bytesRead = count;
		return CEMU_EMBED_OK;
	}
	catch (...) { return CEMU_EMBED_STORAGE_FAILED; }
}

void __cdecl Cemu_UWP_HostMain::CloseBrokeredStream(void*, void* streamHandle)
{
	delete static_cast<BrokeredStream*>(streamHandle);
}

void __cdecl Cemu_UWP_HostMain::BrokeredProgress(void* userData, uint64_t bytesCopied,
	uint64_t totalBytes, const char* relativePath)
{
	auto self = static_cast<Cemu_UWP_HostMain*>(userData);
	if (self && self->m_progressCallback)
		self->m_progressCallback(bytesCopied, totalBytes, relativePath ? relativePath : "");
}

void Cemu_UWP_HostMain::Pump() { if (m_instance) CemuEmbed_Pump(m_instance); }

void Cemu_UWP_HostMain::Stop()
{
	if (!m_instance) return;
	CemuEmbed_Destroy(m_instance);
	m_instance = nullptr;
	m_started = false;
}

void __cdecl Cemu_UWP_HostMain::Log(void*, const char* category, const char* message)
{
	std::string line = "[Cemu";
	if (category && *category)
	{
		line += "/";
		line += category;
	}
	line += "] ";
	if (message) line += message;
	line += "\r\n";
	OutputDebugStringA(line.c_str());
}

void __cdecl Cemu_UWP_HostMain::Error(void* userData, CemuEmbedResult result, const char* message)
{
	std::string line = "[Cemu error ";
	line += std::to_string(static_cast<int>(result));
	line += "] ";
	if (message) line += message;
	line += "\r\n";
	OutputDebugStringA(line.c_str());
	auto self = static_cast<Cemu_UWP_HostMain*>(userData);
	if (!self) return;
	if (self->m_diagnosticCallback) self->m_diagnosticCallback(line);
	if (result == CEMU_EMBED_INITIALIZATION_FAILED)
	{
		self->m_ready.store(false, std::memory_order_release);
		if (self->m_stateCallback) self->m_stateCallback(CEMU_EMBED_STATE_FAILED);
	}
}
void __cdecl Cemu_UWP_HostMain::StateChanged(void* userData, CemuEmbedState state)
{
	auto self = static_cast<Cemu_UWP_HostMain*>(userData);
	if (!self) return;
	self->m_ready.store(state == CEMU_EMBED_STATE_READY, std::memory_order_release);
	if (self->m_stateCallback) self->m_stateCallback(state);
}
