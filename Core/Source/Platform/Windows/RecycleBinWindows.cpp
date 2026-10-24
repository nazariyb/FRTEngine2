#include "Platform/FileSystem.h"

#include <system_error>
#include <thread>
#include <utility>

#include "Platform/Windows/WindowsCommon.h"

#include <ShObjIdl.h>
#include <shellapi.h>
#include <wrl/client.h>


namespace frt::platform
{
namespace
{
using Microsoft::WRL::ComPtr;

/**
 * Watches the one delete IFileOperation performs, for two answers the operation's own result does
 * not give: whether the shell meant to recycle the item, and whether it did.
 *
 * With FOF_NO_UI the shell deletes an item it cannot recycle (no Recycle Bin on the volume, an item
 * too large for it) permanently and silently - the confirmation it would have asked is answered
 * "yes". PreDeleteItem sees TSF_DELETE_RECYCLE_IF_POSSIBLE only when the item is going to the Bin,
 * so refusing every other delete there keeps MoveToTrash from ever destroying data. PostDeleteItem
 * hands over the item now in the Recycle Bin, or nullptr when the item was deleted for good
 * (IFileOperationProgressSink::PostDeleteItem, psiNewlyCreated).
 *
 * Lives on the stack for one PerformOperations call, so the reference count is a formality - as
 * long as it outlives the operation that holds it (RecycleOnThisThread declares it first).
 */
class CRecycleWatcher final : public IFileOperationProgressSink
{
public:
	HRESULT STDMETHODCALLTYPE QueryInterface (REFIID Riid, void** Object) override
	{
		if (Riid == __uuidof(IFileOperationProgressSink) || Riid == __uuidof(IUnknown))
		{
			*Object = static_cast<IFileOperationProgressSink*>(this);
			AddRef();
			return S_OK;
		}
		*Object = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef () override { return ++RefCount; }
	ULONG STDMETHODCALLTYPE Release () override { return --RefCount; }

	HRESULT STDMETHODCALLTYPE PreDeleteItem (DWORD Flags, IShellItem*) override
	{
		bDeleteSeen = true;
		bWouldRecycle = (Flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) != 0u;
		return bWouldRecycle ? S_OK : HRESULT_FROM_WIN32(ERROR_CANCELLED);
	}

	HRESULT STDMETHODCALLTYPE PostDeleteItem (DWORD, IShellItem*, HRESULT Result, IShellItem* NewlyCreated) override
	{
		bRecycled = SUCCEEDED(Result) && NewlyCreated != nullptr;
		return S_OK;
	}

	// Nothing to watch in the rest.
	HRESULT STDMETHODCALLTYPE StartOperations () override { return S_OK; }
	HRESULT STDMETHODCALLTYPE FinishOperations (HRESULT) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PreRenameItem (DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostRenameItem (DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PreMoveItem (DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostMoveItem (DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PreCopyItem (DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostCopyItem (DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PreNewItem (DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PostNewItem (DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE UpdateProgress (UINT, UINT) override { return S_OK; }
	HRESULT STDMETHODCALLTYPE ResetTimer () override { return S_OK; }
	HRESULT STDMETHODCALLTYPE PauseTimer () override { return S_OK; }
	HRESULT STDMETHODCALLTYPE ResumeTimer () override { return S_OK; }

	bool bDeleteSeen = false;
	bool bWouldRecycle = false;
	bool bRecycled = false;

private:
	ULONG RefCount = 1u;
};

// The delete itself, on a thread in a single-threaded apartment.
bool RecycleOnThisThread (const std::filesystem::path& Path, FString* OutError)
{
	// The shell parses absolute paths only.
	std::error_code ec;
	const std::filesystem::path absolute = std::filesystem::absolute(Path, ec);

	ComPtr<IShellItem> item;
	HRESULT result = SHCreateItemFromParsingName(absolute.c_str(), nullptr, IID_PPV_ARGS(&item));
	if (FAILED(result))
	{
		windows::SetErrorHresult(OutError, "Cannot find", Path, result);
		return false;
	}

	// Declared before the operation, so it outlives every reference the operation holds: the
	// operation may keep the sink until it is destroyed itself, even after Unadvise (or when
	// Unadvise fails), and its last Release must not reach a destroyed object.
	CRecycleWatcher watcher;
	ComPtr<IFileOperation> operation;
	result = CoCreateInstance(__uuidof(FileOperation), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&operation));
	if (SUCCEEDED(result))
	{
		// No dialogs of any kind; recycle instead of deleting; stop at the first error.
		result = operation->SetOperationFlags(FOF_NO_UI | FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOFX_EARLYFAILURE);
	}
	if (FAILED(result))
	{
		windows::SetErrorHresult(OutError, "Cannot prepare moving to the Recycle Bin", Path, result);
		return false;
	}

	DWORD cookie = 0u;
	const bool bAdvised = SUCCEEDED(operation->Advise(&watcher, &cookie));
	if (!bAdvised)
	{
		// Without the watcher a permanent delete could not be refused: do not delete at all.
		windows::SetErrorText(OutError, "Cannot move to the Recycle Bin", Path, "the shell refused the progress sink");
		return false;
	}

	result = operation->DeleteItem(item.Get(), nullptr);
	if (SUCCEEDED(result))
	{
		result = operation->PerformOperations();
	}
	// A failure here only means the operation keeps the sink until it goes: harmless (see above).
	operation->Unadvise(cookie);

	if (watcher.bDeleteSeen && !watcher.bWouldRecycle)
	{
		windows::SetErrorText(OutError, "Cannot move to the Recycle Bin", Path,
			"this volume has no Recycle Bin, or the item is too large for it; nothing was deleted");
		return false;
	}
	BOOL bAborted = FALSE;
	operation->GetAnyOperationsAborted(&bAborted);
	if (FAILED(result) || bAborted || !watcher.bRecycled)
	{
		windows::SetErrorHresult(OutError, "Cannot move to the Recycle Bin", Path, FAILED(result) ? result : E_FAIL);
		return false;
	}
	return true;
}
}


bool MoveToTrash (const std::filesystem::path& Path, FString* OutError)
{
	const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	if (init == RPC_E_CHANGED_MODE)
	{
		// This thread is in the multithreaded apartment, where IFileOperation does not work: a thread
		// of its own, which initializes its own apartment.
		bool bResult = false;
		FString error;
		try
		{
			std::thread helper([&Path, &bResult, &error] { bResult = MoveToTrash(Path, &error); });
			helper.join();
		}
		catch (const std::system_error&)
		{
			error = "Cannot start a thread to move to the Recycle Bin";
		}
		if (!bResult && OutError != nullptr)
		{
			*OutError = std::move(error);
		}
		return bResult;
	}
	if (FAILED(init))
	{
		windows::SetErrorHresult(OutError, "Cannot initialize COM to move to the Recycle Bin", Path, init);
		return false;
	}

	const bool bResult = RecycleOnThisThread(Path, OutError);
	// S_OK and S_FALSE (already initialized as an STA) both take one CoUninitialize.
	CoUninitialize();
	return bResult;
}
}
