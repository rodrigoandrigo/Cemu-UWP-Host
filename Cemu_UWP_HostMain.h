#pragma once

#include "Cemu/CemuEmbed.h"
#include <atomic>
#include <functional>
#include <string>
#include <vector>

// Owns the embedded Cemu runtime for the lifetime of the UWP page.
namespace Cemu_UWP_Host
{
	struct InstalledTitle
	{
		uint64_t titleId{};
		uint16_t baseVersion{};
		uint16_t effectiveVersion{};
		uint16_t updateVersion{};
		uint16_t dlcVersion{};
		uint32_t dlcCount{};
		uint32_t region{};
		uint32_t compatibleGraphicPackCount{};
		uint32_t enabledGraphicPackCount{};
		std::string name;
		std::string regionName;
		std::string localGamePath;
		std::string localGameFormat;
		std::string brokeredRelativePath;
		Windows::Storage::StorageFolder^ brokeredTitleFolder{ nullptr };
		bool isExternalStorage{};
		uint64_t graphicPackTitleId{};
		std::vector<uint8_t> iconTga;
	};

	struct ActiveAccount
	{
		uint32_t persistentId{};
		bool onlineEnabled{};
		std::string miiName;
		std::string accountId;
	};

	struct DimensionsFigure
	{
		uint32_t id{};
		bool vehicleOrGadget{};
		std::string name;
	};

	struct GraphicPack
	{
		std::string identity;
		std::string name;
		std::string category;
		std::string description;
		bool enabled{};
		bool defaultEnabled{};
	};

	class Cemu_UWP_HostMain
	{
	public:
		Cemu_UWP_HostMain(HWND window, const CemuEmbedD3D11Surface& surface, int width, int height, double dpiScale);
		~Cemu_UWP_HostMain();
		bool Start();
		bool LaunchGame(Windows::Storage::StorageFolder^ gameFolder);
		bool LaunchGameFile(Windows::Storage::StorageFile^ gameFile);
		bool LaunchGamePath(const std::string& gamePath);
		bool LaunchExternalGamePath(const std::string& gamePath,
			const std::vector<std::string>& supplementalTitlePaths);
		bool LaunchExternalGameFolders(Windows::Storage::StorageFolder^ selectedFolder,
			const std::string& selectedRelativePath,
			const std::vector<Windows::Storage::StorageFolder^>& supplementalFolders);
		bool IdentifyGamePath(const std::string& gamePath, uint64_t* baseTitleId);
		bool IdentifyBrokeredGame(Windows::Storage::StorageFolder^ folder,
			const std::string& selectedRelativePath, uint64_t* baseTitleId);
		bool InstallTitle(Windows::Storage::StorageFolder^ titleFolder,
			CemuEmbedInstallType expectedType, uint64_t* installedBaseTitleId = nullptr);
		std::vector<InstalledTitle> GetInstalledTitles();
		bool GetActiveAccount(ActiveAccount& account);
		bool LaunchInstalledTitle(uint64_t baseTitleId);
		bool DeleteInstalledTitle(uint64_t baseTitleId,
			uint32_t* removedInstallFolderCount = nullptr);
		bool DownloadGraphicPacks(uint32_t* downloadedPackCount = nullptr,
			bool* alreadyCurrent = nullptr);
		bool ClearShaderCaches(uint32_t* removedEntryCount = nullptr);
		bool GetShaderCount(uint64_t baseTitleId, uint32_t* shaderCount);
		bool InstallGraphicPacks(Windows::Storage::StorageFolder^ graphicPacksFolder,
			uint32_t* importedPackCount = nullptr);
		bool SetGraphicPacksEnabledForTitle(uint64_t baseTitleId, bool enabled,
			uint32_t* affectedPackCount = nullptr);
		std::vector<GraphicPack> GetGraphicPacksForTitle(uint64_t baseTitleId);
		bool SetGraphicPackEnabled(uint64_t baseTitleId,
			const std::string& identity, bool enabled);
		bool ApplySafeGraphicPackPolicyForTitle(uint64_t baseTitleId,
			uint32_t* affectedPackCount = nullptr);
		bool EnsureDefaultGamepadProfile();
		bool SetGamepadState(const CemuEmbedGamepadState& state);
		bool SetVirtualMouse(int x, int y, bool leftDown, bool enabled);
		bool SetPerformanceMetrics(bool enabled);
		bool GetSettings(CemuEmbedSettings& settings);
		bool SetSettings(const CemuEmbedSettings& settings);
		std::vector<DimensionsFigure> GetDimensionsFigures();
		bool PlaceDimensionsFigure(uint32_t figureId, uint8_t slot);
		bool RemoveDimensionsFigure(uint8_t slot);
		bool MoveDimensionsFigure(uint8_t sourceSlot, uint8_t destinationSlot);
		bool ImportKeys(const std::vector<uint8_t>& data, uint32_t* validKeyCount = nullptr);
		void SetDiagnosticCallback(std::function<void(const std::string&)> callback);
		void SetStateCallback(std::function<void(CemuEmbedState)> callback);
		void SetProgressCallback(std::function<void(uint64_t, uint64_t, const std::string&)> callback);
		bool IsReady() const;
		void ReplaceD3D11Surface(const CemuEmbedD3D11Surface& surface);
		void ResizeSurface(int width, int height, double dpiScale);
		void Pump();
		void Stop();

	private:
		static void __cdecl Log(void* userData, const char* category, const char* message);
		static void __cdecl Error(void* userData, CemuEmbedResult result, const char* message);
		static void __cdecl StateChanged(void* userData, CemuEmbedState state);
		static CemuEmbedResult __cdecl EnumerateBrokeredFolder(void* userData, void* folderHandle, CemuEmbedBrokeredEntryCallback entryCallback, void* entryCallbackUserData);
		static CemuEmbedResult __cdecl OpenBrokeredFile(void* userData, void* fileHandle, void** streamHandle);
		static CemuEmbedResult __cdecl OpenBrokeredRelativeFile(void* userData, void* folderHandle, const char* relativePath, void** streamHandle);
		static CemuEmbedResult __cdecl ReadBrokeredStream(void* userData, void* streamHandle, uint64_t offset, uint8_t* buffer, uint32_t bufferSize, uint32_t* bytesRead);
		static void __cdecl CloseBrokeredStream(void* userData, void* streamHandle);
		static void __cdecl BrokeredProgress(void* userData, uint64_t bytesCopied, uint64_t totalBytes, const char* relativePath);
		static CemuEmbedResult __cdecl CopyBrokeredFileToCache(void* userData, void* fileHandle, const char* destinationPathUtf8);
		static void __cdecl DirectCopyBrokeredProgress(void* userData, uint64_t bytesCopied, uint64_t totalBytes, const char* relativePath);
		static CemuEmbedResult __cdecl InstalledTitleFound(void* userData,
			const CemuEmbedInstalledTitle* title);
		static CemuEmbedResult __cdecl DimensionsFigureFound(void* userData,
			const CemuEmbedDimensionsFigure* figure);
		static CemuEmbedResult __cdecl GraphicPackFound(void* userData,
			const CemuEmbedGraphicPack* graphicPack);
		CemuEmbedInstance* m_instance = nullptr;
		HWND m_window = nullptr;
		CemuEmbedD3D11Surface m_d3d11Surface{};
		int m_width = 0;
		int m_height = 0;
		double m_dpiScale = 1.0;
		bool m_started = false;
		std::atomic_bool m_ready{false};
		std::function<void(const std::string&)> m_diagnosticCallback;
		std::function<void(CemuEmbedState)> m_stateCallback;
		std::function<void(uint64_t, uint64_t, const std::string&)> m_progressCallback;
		// Keep WinRT folder capabilities alive while the core services on-demand
		// reads from removable/network storage.
		std::vector<Windows::Storage::StorageFolder^> m_activeExternalFolders;
	};
}
