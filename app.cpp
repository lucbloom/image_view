// ImageViewer.cpp
// Single-file minimalist Win32 app.
// Build (MSVC): cl /EHsc /Zi ImageViewer.cpp /link gdiplus.lib comctl32.lib shell32.lib ole32.lib

#define WINDOWS_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <Shlwapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <chrono>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "Shlwapi.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

static ULONG_PTR g_gdiplusToken;
static HWND g_hMain = nullptr;
static HWND g_hNext, g_hPrev, g_hOpenPS, g_hOpenPN, g_hShowInExplorer, g_hToggle100, g_hToggleRec, g_hRotate, g_hCopy, g_hDelete, g_hInfo;
static HWND g_hPanel;
static HWND g_hChangeRoot = nullptr;
static HINSTANCE g_hInst;
static wchar_t g_rootPath[MAX_PATH] = { 0 };
static std::atomic<bool> g_recursive{ false };
static std::vector<fs::path> g_files;
static std::mutex g_filesMutex;
static std::atomic<int> g_index{ 0 };
static std::map<std::wstring, std::shared_ptr<Bitmap>> g_cache;
static std::mutex g_cacheMutex;
static std::atomic<bool> g_loading{ false };
static std::atomic<DWORD> g_zoom{ 2 };
static std::atomic<bool> g_stopThreads{ false };
static int g_frameIndex = 0;

static void ChooseRootDirectory()
{
	// Use IFileDialog to pick a folder (modern folder picker)
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	IFileDialog* pfd = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd))))
	{
		DWORD dwOptions;
		if (SUCCEEDED(pfd->GetOptions(&dwOptions)))
		{
			pfd->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		}
		if (SUCCEEDED(pfd->Show(g_hMain)))
		{
			IShellItem* psi = nullptr;
			if (SUCCEEDED(pfd->GetResult(&psi)) && psi)
			{
				PWSTR pszPath = nullptr;
				if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath)
				{
					wcsncpy_s(g_rootPath, pszPath, _TRUNCATE);
					RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ImageViewer", L"RootPath", REG_SZ, g_rootPath, (DWORD)((wcslen(g_rootPath) + 1) * sizeof(wchar_t)));
					CoTaskMemFree(pszPath);
				}
				psi->Release();
			}
		}
		pfd->Release();
	}
	CoUninitialize();
}

// helpers
static inline bool has_ext(const fs::path& p)
{
	auto e = p.extension().wstring();
	for (auto& c : e) c = towlower(c);
	return e == L".jpg" || e == L".jpeg" || e == L".png" || e == L".bmp" || e == L".ico" || e == L".gif";
}

static CLSID GetEncoderClsid(const WCHAR* format)
{
	UINT  num = 0;
	UINT  size = 0;
	ImageCodecInfo* pImageCodecInfo = nullptr;
	GetImageEncodersSize(&num, &size);
	if (size == 0) return CLSID();
	pImageCodecInfo = (ImageCodecInfo*)(malloc(size));
	GetImageEncoders(num, size, pImageCodecInfo);
	for (UINT j = 0; j < num; ++j)
	{
		if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
		{
			CLSID id = pImageCodecInfo[j].Clsid;
			free(pImageCodecInfo);
			return id;
		}
	}
	free(pImageCodecInfo);
	return CLSID();
}

static std::wstring RawFormatToType(const GUID& g)
{
	// commonly-used GUIDs
	if (g == ImageFormatJPEG) return L"JPEG";
	if (g == ImageFormatPNG) return L"PNG";
	if (g == ImageFormatBMP) return L"BMP";
	if (g == ImageFormatIcon) return L"ICO";
	if (g == ImageFormatGIF) return L"GIF";
	return L"Unknown";
}

static void EnumFiles(const std::wstring& root, bool recursive)
{
	std::vector<fs::path> files;
	try
	{
		if (recursive)
		{
			for (auto& p : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied))
			{
				if (p.is_regular_file() && has_ext(p.path())) files.push_back(p.path());
			}
		}
		else
		{
			for (auto& p : fs::directory_iterator(root))
			{
				if (p.is_regular_file() && has_ext(p.path())) files.push_back(p.path());
			}
		}
	}
	catch (...) {}
	std::lock_guard<std::mutex> lk(g_filesMutex);
	g_files = move(files);
	if (g_files.empty()) g_index = 0;
	else if (g_index >= (int)g_files.size()) g_index = 0;
}

static void PreloadAround(int idx)
{
	std::lock_guard<std::mutex> lk(g_cacheMutex);
	if (g_files.empty()) return;
	int n = (int)g_files.size();
	auto loadOne = [&](int i)
		{
			i = (i % n + n) % n;
			auto p = g_files[i].wstring();
			if (g_cache.count(p)) return;
			Bitmap* bmp = Bitmap::FromFile(p.c_str(), FALSE);
			if (bmp && bmp->GetLastStatus() == Ok)
			{
				g_cache[p] = std::shared_ptr<Bitmap>(bmp);
			}
			else delete bmp;
		};
	// preload idx-2..idx+2
	for (int d = -2; d <= 2; ++d) loadOne(idx + d);
	// trim cache to a small size (keep max 9)
	while (g_cache.size() > 12)
	{
		auto it = g_cache.begin();
		g_cache.erase(it);
	}
}

static std::wstring GetPropertyString(Bitmap* img, PROPID id)
{
	// returns raw property item as string or "-" if not present
	UINT len = img->GetPropertyItemSize(id);
	if (!len) return L"-";
	std::vector<BYTE> buf(len);
	PropertyItem* pi = (PropertyItem*)buf.data();
	img->GetPropertyItem(id, len, pi);
	// handle some common types (ascii)
	if (pi->type == PropertyTagTypeASCII)
	{
		std::string s((char*)pi->value, pi->length);
		// convert
		int sz = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
		std::wstring ws(sz, 0);
		MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &ws[0], sz);
		// remove trailing \0 extra
		if (!ws.empty() && ws.back() == '\0') ws.pop_back();
		return ws.empty() ? L"-" : ws;
	}
	return L"-";
}

static int GetExifOrientation(Bitmap* img)
{
	UINT len = img->GetPropertyItemSize(PropertyTagOrientation);
	if (!len) return 1;
	std::vector<BYTE> buf(len);
	PropertyItem* pi = (PropertyItem*)buf.data();
	if (img->GetPropertyItem(PropertyTagOrientation, len, pi) != Ok) return 1;
	if (pi->type == PropertyTagTypeShort)
	{
		return *((WORD*)pi->value);
	}
	return 1;
}

static void SetExifOrientation(const std::wstring& file, int val)
{
	// load image, set orientation property item and save to temp then replace file
	Bitmap* img = Bitmap::FromFile(file.c_str(), FALSE);
	if (!img) return;
	UINT len = img->GetPropertyItemSize(PropertyTagOrientation);
	PropertyItem* pi = nullptr;
	if (len)
	{
		std::vector<BYTE> buf(len);
		pi = (PropertyItem*)buf.data();
		img->GetPropertyItem(PropertyTagOrientation, len, pi);
		// modify
		if (pi->type == PropertyTagTypeShort)
		{
			*((WORD*)pi->value) = (WORD)val;
			// save into temp
			CLSID cls = GetEncoderClsid(L"image/jpeg");
			std::wstring tmp = file + L".tmp_exif";
			Status s = img->Save(tmp.c_str(), &cls, nullptr);
			if (s == Ok)
			{
				MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
			}
		}
	}
	delete img;
}

static std::shared_ptr<Bitmap> GetBitmapAt(int idx)
{
	std::wstring p;
	{
		std::lock_guard<std::mutex> lk(g_filesMutex);
		if (g_files.empty()) return nullptr;

		int n = (int)g_files.size();
		idx = (idx % n + n) % n;
		p = g_files[idx].wstring();
	}

	{
		std::lock_guard<std::mutex> clk(g_cacheMutex);
		auto it = g_cache.find(p);
		if (it != g_cache.end()) return it->second;
	}

	// not cached, load synchronously here (used rarely)
	Bitmap* b = Bitmap::FromFile(p.c_str(), FALSE);
	if (b && b->GetLastStatus() == Ok)
	{
		auto sp = std::shared_ptr<Bitmap>(b);
		{
			std::lock_guard<std::mutex> clk(g_cacheMutex);
			g_cache[p] = sp;
		}
		return sp;
	}
	delete b;
	return nullptr;
}

static void BackgroundLoader()
{
	while (!g_stopThreads)
	{
		if (!g_loading)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		int idx = g_index;
		PreloadAround(idx);
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}
}

static void StartBackground()
{
	g_stopThreads = false;
	std::thread([]() { BackgroundLoader(); }).detach();
}

static void StopBackground()
{
	g_stopThreads = true;
}

static void CalcRectAndMatrix(UINT w, UINT h, int orient, RECT rc, Matrix& mx, Rect& rect)
{
	// compute displayed size
	rect.Width = w;
	rect.Height = h;
	double imgAspect = (double)w / h;
	double ww = rc.right - rc.left;
	double wh = rc.bottom - rc.top;
	rect.X = rc.left;
	rect.Y = rc.top;
	if (g_zoom == 0 || (g_zoom == 2 && (w <= ww && h <= wh)))
	{
		rect.X += (int)((ww - rect.Width) / 2.0);
		rect.Y += (int)((wh - rect.Height) / 2.0);
	}
	else
	{
		double wAR = ww / wh;
		if (imgAspect > wAR)
		{
			rect.Width = (int)ww;
			rect.Height = (int)(wh / imgAspect);
			rect.Y += (int)((wh - rect.Height) / 2.0);
		}
		else
		{
			rect.Height = (int)wh;
			rect.Width = (int)(wh * imgAspect);
			rect.X += (int)((ww - rect.Width) / 2.0);
		}
	}

	// draw with orientation applied
	// convert to rotated matrix if needed
	switch (orient)
	{
	case 2: mx.Scale(-1.0f, 1.0f); mx.Translate((float)rect.Width, 0); break;
	case 3: mx.RotateAt(180.0f, PointF((float)(rect.X + rect.Width / 2.0f), (float)(rect.Y + rect.Height / 2.0f))); break;
	case 4: mx.Scale(1.0f, -1.0f); mx.Translate(0, (float)rect.Height); break;
	case 5: mx.RotateAt(90.0f, PointF((float)(rect.X + rect.Width / 2.0f), (float)(rect.Y + rect.Height / 2.0f))); mx.Scale(1.0f, -1.0f); break;
	case 6: mx.RotateAt(90.0f, PointF((float)(rect.X + rect.Width / 2.0f), (float)(rect.Y + rect.Height / 2.0f))); break;
	case 7: mx.RotateAt(270.0f, PointF((float)(rect.X + rect.Width / 2.0f), (float)(rect.Y + rect.Height / 2.0f))); mx.Scale(1.0f, -1.0f); break;
	case 8: mx.RotateAt(270.0f, PointF((float)(rect.X + rect.Width / 2.0f), (float)(rect.Y + rect.Height / 2.0f))); break;
	default: break;
	}
}

void ClearCheckeredBackground(Gdiplus::Graphics& g, RECT rc, int tileSize = 16)
{
	Gdiplus::SolidBrush light(Gdiplus::Color(255, 30, 30, 30));
	Gdiplus::SolidBrush dark(Gdiplus::Color(255, 40, 40, 40));

	for (int y = rc.top; y < rc.bottom; y += tileSize)
	{
		for (int x = rc.left; x < rc.right; x += tileSize)
		{
			bool isLight = ((x / tileSize) + (y / tileSize)) % 2 == 0;
			g.FillRectangle(isLight ? &light : &dark, x, y, tileSize + 1, tileSize + 1);
		}
	}
}

static std::shared_ptr<Gdiplus::Bitmap> g_backBuffer;

static void DrawImageOntoBackbuffer(RECT rc)
{
	Gdiplus::Graphics g(g_backBuffer.get());
	g.SetSmoothingMode(SmoothingModeHighQuality);
	g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

	// clear background
	ClearCheckeredBackground(g, rc);

	// Utility label
	Gdiplus::Font font(L"Segoe UI", 18);
	Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 0, 0));
	Gdiplus::RectF layout((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left), (REAL)(rc.bottom - rc.top));

	if (g_files.empty())
	{
		// no files found
		g.DrawString(L"No image found", -1, &font, layout, nullptr, &brush);
		return;
	}

	std::shared_ptr<Bitmap> bmp = GetBitmapAt(g_index);
	if (!bmp)
	{
		// file exists but failed to load
		g.DrawString(L"Error loading image", -1, &font, layout, nullptr, &brush);
		return;
	}

	Matrix mx;
	Rect dst;
	CalcRectAndMatrix(bmp->GetWidth(), bmp->GetHeight(), GetExifOrientation(bmp.get()), rc, mx, dst);
	g.SetTransform(&mx);
	g.DrawImage(bmp.get(), dst);
}

static void DrawBackbufferOntoScreen(HWND hWnd, RECT rc)
{
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hWnd, &ps);
	Gdiplus::Graphics gdc(hdc);
	gdc.DrawImage(g_backBuffer.get(), 0, 0, rc.right - rc.left, rc.bottom - rc.top);
	EndPaint(hWnd, &ps);
}

static void PaintImage(HWND hWnd)
{
	RECT rc;
	GetClientRect(hWnd, &rc);

	if (!g_backBuffer || g_backBuffer->GetWidth() != (UINT)(rc.right - rc.left) || g_backBuffer->GetHeight() != (UINT)(rc.bottom - rc.top))
	{
		g_backBuffer = std::make_shared<Gdiplus::Bitmap>(rc.right - rc.left, rc.bottom - rc.top, PixelFormat32bppARGB);
	}

	DrawImageOntoBackbuffer(rc);
	DrawBackbufferOntoScreen(hWnd, rc);
}

static void GetFileTimes(const std::wstring& p, std::wstring& created, std::wstring& modified)
{
	FILETIME ftCreate, ftWrite;
	HANDLE hf = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hf != INVALID_HANDLE_VALUE)
	{
		if (GetFileTime(hf, &ftCreate, NULL, &ftWrite))
		{
			SYSTEMTIME ct = { 0 }, mt = { 0 };
			FileTimeToSystemTime(&ftCreate, &ct);
			FileTimeToSystemTime(&ftWrite, &mt);
			wchar_t buf[64];
			swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d", ct.wYear, ct.wMonth, ct.wDay, ct.wHour, ct.wMinute, ct.wSecond);
			created = buf;
			swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d", mt.wYear, mt.wMonth, mt.wDay, mt.wHour, mt.wMinute, mt.wSecond);
			modified = buf;
		}
		CloseHandle(hf);
	}
}

static void GetBitmapInfo(const std::wstring& p, UINT& w, UINT& h, UINT& bpp, std::wstring& type, ULONGLONG& fsize, std::wstring& exifDate)
{
	auto bmp = GetBitmapAt(g_index);
	if (bmp)
	{
		w = bmp->GetWidth();
		h = bmp->GetHeight();
		PixelFormat pf = bmp->GetPixelFormat();
		bpp = (pf & PixelFormatIndexed) ? 8 : GetPixelFormatSize(pf);
		GUID g;
		bmp->GetRawFormat(&g);
		type = RawFormatToType(g);
		exifDate = GetPropertyString(bmp.get(), PropertyTagDateTime);
	}
	try { fsize = fs::file_size(p); }
	catch (...) {}
}

static void LoadNextBitmap()
{
	auto p = g_files[g_index];

	UINT w = 0, h = 0, bpp = 0;
	std::wstring type = L"-";
	ULONGLONG fsize = 0;
	std::wstring exifDate = L"-";
	GetBitmapInfo(p, w, h, bpp, type, fsize, exifDate);

	std::wstring created = L"-", modified = L"-";
	GetFileTimes(p, created, modified);

	wchar_t buf[1024];
	swprintf(buf, 1024,
		L"Name: %s\r\nType: %s\r\nSize: %I64u bytes\r\nDimensions: %u x %u\r\nBPP: %u\r\nFull path: %s\r\nCurrent root: %s\r\nCreated: %s\r\nModified: %s\r\nEXIF captured: %s",
		p.filename().wstring().c_str(),
		type.c_str(),
		fsize,
		w, h, bpp,
		p.wstring().c_str(),
		g_rootPath[0] ? g_rootPath : L".",
		created.c_str(),
		modified.c_str(),
		exifDate.c_str()
	);
	SetWindowTextW(g_hInfo, buf);
}

static void UpdateInfoLabel()
{
	{
		std::lock_guard<std::mutex> lk(g_filesMutex);
		if (g_files.empty())
		{
			SetWindowTextW(g_hInfo, L"No images");
			return;
		}
	}
	LoadNextBitmap();
}

static void DoOpenWith(const std::wstring& exeHint)
{
	fs::path file;
	{
		std::lock_guard<std::mutex> lk(g_filesMutex);
		if (g_files.empty()) { return; }
		file = g_files[g_index];
	}

	// try candidate locations
	std::wstring candidates[] = {
		exeHint,
		L"C:\\Program Files\\Adobe\\Adobe Photoshop 2023\\Photoshop.exe",
		L"C:\\Program Files\\Adobe\\Adobe Photoshop 2024\\Photoshop.exe",
		L"C:\\Program Files\\Adobe\\Adobe Photoshop 2022\\Photoshop.exe",
		L"C:\\Program Files\\paint.net\\PaintDotNet.exe",
		L"C:\\Program Files (x86)\\paint.net\\PaintDotNet.exe"
	};
	// first try exeHint as verb (open with application)
	for (auto& c : candidates)
	{
		if (c.empty()) continue;
		if (PathFileExistsW(c.c_str()))
		{
			SHELLEXECUTEINFOW si = { sizeof(si) };
			si.fMask = SEE_MASK_NOCLOSEPROCESS;
			si.lpVerb = L"open";
			si.lpFile = c.c_str();
			auto arg = file.wstring();
			si.lpParameters = arg.c_str();
			ShellExecuteExW(&si);
			return;
		}
	}
	// fallback: use "open" with file association (will open default)
	ShellExecuteW(NULL, L"open", file.wstring().c_str(), NULL, NULL, SW_SHOWNORMAL);
}

static void CopyToClipboardAsGif(const fs::path& file)
{
	// save current image as GIF to temp and put path in clipboard as HDROP (so user can paste file)
	std::wstring tmp = fs::temp_directory_path().wstring() + L"\\iv_tmp.gif";
	std::shared_ptr<Bitmap> bmp = GetBitmapAt(g_index);
	if (!bmp) return;
	CLSID cls = GetEncoderClsid(L"image/gif");
	bmp->Save(tmp.c_str(), &cls, nullptr);
	// put file path on clipboard as CF_HDROP
	//OPENFILENAMEW ofn; // not used, just prepare
	HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, (tmp.size() + 2) * sizeof(wchar_t));
	if (!hMem) return;
	wchar_t* p = (wchar_t*)GlobalLock(hMem);
	wcscpy_s(p, tmp.size() + 2, tmp.c_str());
	GlobalUnlock(hMem);
	// prepare DROPFILES structure
	size_t pathLen = tmp.size() + 1;
	size_t total = sizeof(DROPFILES) + (pathLen + 1) * sizeof(wchar_t);
	HGLOBAL hg = GlobalAlloc(GHND | GMEM_MOVEABLE, total);
	if (!hg) return;
	void* pv = GlobalLock(hg);
	DROPFILES* df = (DROPFILES*)pv;
	df->pFiles = sizeof(DROPFILES);
	df->pt.x = df->pt.y = 0;
	df->fWide = TRUE;
	wchar_t* dst = (wchar_t*)((char*)pv + sizeof(DROPFILES));
	wcscpy_s(dst, pathLen + 1, tmp.c_str());
	GlobalUnlock(hg);
	if (OpenClipboard(nullptr))
	{
		EmptyClipboard();
		SetClipboardData(CF_HDROP, hg);
		CloseClipboard();
	}
}

static void DeleteCurrent()
{
	std::lock_guard<std::mutex> lk(g_filesMutex);
	if (g_files.empty()) return;
	auto p = g_files[g_index];
	WCHAR buf[512];
	swprintf(buf, 512, L"Delete %s?", p.filename().wstring().c_str());
	if (MessageBoxW(g_hMain, buf, L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES)
	{
		// move to recycle bin
		SHFILEOPSTRUCTW fo = { 0 };
		fo.wFunc = FO_DELETE;
		fo.pFrom = p.c_str();
		fo.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
		SHFileOperationW(&fo);
		EnumFiles(g_rootPath[0] ? g_rootPath : fs::current_path().wstring(), g_recursive);
	}
}

static void Rotate90AndResave(bool clockwise)
{
	{
		std::lock_guard<std::mutex> lk(g_filesMutex);
		if (g_files.empty()) return;
	}

	std::shared_ptr<Bitmap> bmp = GetBitmapAt(g_index);
	if (!bmp) return;

	// rotate
	bmp->RotateFlip(clockwise ? Rotate90FlipNone : Rotate270FlipNone);
	// write back to file (attempt to preserve metadata by saving as same format)
	GUID rf; bmp->GetRawFormat(&rf);
	CLSID enc = ImageFormatJPEG;
	if (rf == ImageFormatPNG) enc = GetEncoderClsid(L"image/png");
	else if (rf == ImageFormatBMP) enc = GetEncoderClsid(L"image/bmp");
	else if (rf == ImageFormatGIF) enc = GetEncoderClsid(L"image/gif");
	else enc = GetEncoderClsid(L"image/jpeg");

	{
		std::lock_guard<std::mutex> lk(g_filesMutex);
		auto p = g_files[g_index];
		std::wstring tmp = p.wstring() + L".tmp";
		if (bmp->Save(tmp.c_str(), &enc, nullptr) == Ok)
		{
			MoveFileExW(tmp.c_str(), p.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
			// update exif orientation to 1 (normal)
			SetExifOrientation(p.wstring(), 1);
			// reload cache for this file
			std::lock_guard<std::mutex> clk(g_cacheMutex);
			g_cache.erase(p.wstring());
		}
	}
}

static void OpenInExplorer()
{
	fs::path file;
	{
		std::lock_guard<std::mutex> lk(g_filesMutex);
		if (g_files.empty()) { return; }
		file = g_files[g_index];
	}
	std::wstring params = L"/select,\"" + file.wstring() + L"\"";
	ShellExecuteW(NULL, L"open", L"explorer.exe", params.c_str(), NULL, SW_SHOWNORMAL);
}

UINT GetFrameDelay(std::shared_ptr<Gdiplus::Bitmap> bmp, UINT frameIndex)
{
	if (!bmp) return 100; // default 100ms

	UINT size = bmp->GetPropertyItemSize(PropertyTagFrameDelay);
	if (!size) return 100;

	PropertyItem* pItem = (PropertyItem*)malloc(size);
	if (!pItem) return 100;

	UINT delayMs = 100; // default 100ms

	if (bmp->GetPropertyItem(PropertyTagFrameDelay, size, pItem) == Ok)
	{
		UINT frameCount = pItem->length / sizeof(UINT);
		UINT* vals = (UINT*)pItem->value;
		if (frameIndex < frameCount)
			delayMs = vals[frameIndex] * 10; // convert 1/100s -> ms
	}

	free(pItem);
	return delayMs;
}

UINT_PTR g_gifTimerId = 10288; // any unique ID
void QueueNextFrame()
{
	if (!g_files.empty())
	{
		std::shared_ptr<Gdiplus::Bitmap> bmp = GetBitmapAt(g_index);
		int fc = bmp ? bmp->GetFrameCount(&FrameDimensionTime) : 1;
		if (fc > 1)
		{
			g_frameIndex = (g_frameIndex + 1) % fc;
			bmp->SelectActiveFrame(&FrameDimensionTime, g_frameIndex);
			InvalidateRect(g_hPanel, nullptr, FALSE);

			UINT delay = GetFrameDelay(bmp, g_frameIndex);
			SetTimer(g_hMain, g_gifTimerId, delay, NULL);
			return;
		}
	}

	KillTimer(g_hMain, g_gifTimerId);
}

static void ShowImageAtIndex(int index)
{
	if (g_files.empty()) return;
	if (index < 0) index = (int)g_files.size() - 1;
	if (index >= g_files.size()) index = 0;
	g_index = index;
	g_frameIndex = 0;
	QueueNextFrame();
	UpdateInfoLabel();
	InvalidateRect(g_hPanel, NULL, TRUE);
}

static void PrevImage() { ShowImageAtIndex(g_index - 1); }
static void NextImage() { ShowImageAtIndex(g_index + 1); }

WNDPROC g_oldPanelProc;
LRESULT CALLBACK PanelProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
		case WM_PAINT:
		{
			PaintImage(hWnd);
			return 0;
		}
	}
	return CallWindowProc(g_oldPanelProc, hWnd, msg, wParam, lParam);
}


LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_CREATE:
	{
		g_hPanel = CreateWindowExW(0, L"STATIC", NULL, WS_CHILD | WS_VISIBLE | BS_NOTIFY, 10, 10, 800, 600, hWnd, NULL, g_hInst, NULL);
		g_oldPanelProc = (WNDPROC)SetWindowLongPtrW(g_hPanel, GWLP_WNDPROC, (LONG_PTR)PanelProc);

		g_hPrev = CreateWindowW(L"BUTTON", L"Prev", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 10, 620, 80, 28, hWnd, (HMENU)101, g_hInst, NULL);
		g_hNext = CreateWindowW(L"BUTTON", L"Next", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 100, 620, 80, 28, hWnd, (HMENU)102, g_hInst, NULL);
		g_hOpenPS = CreateWindowW(L"BUTTON", L"Open with Photoshop", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 200, 620, 160, 28, hWnd, (HMENU)103, g_hInst, NULL);
		g_hOpenPN = CreateWindowW(L"BUTTON", L"Open with Paint.NET", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 370, 620, 160, 28, hWnd, (HMENU)104, g_hInst, NULL);
		g_hShowInExplorer = CreateWindowW(L"BUTTON", L"Show in Explorer", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 540, 620, 130, 28, hWnd, (HMENU)105, g_hInst, NULL);
		g_hToggle100 = CreateWindowW(L"BUTTON", g_zoom == 0 ? L"100%" : (g_zoom == 1 ? L"Fit" : L"Shrink"), WS_CHILD | WS_VISIBLE | BS_NOTIFY, 680, 620, 100, 28, hWnd, (HMENU)106, g_hInst, NULL);
		g_hToggleRec = CreateWindowW(L"BUTTON", g_recursive ? L"Recursive: On" : L"Recursive: Off", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 10, 660, 120, 28, hWnd, (HMENU)107, g_hInst, NULL);
		g_hRotate = CreateWindowW(L"BUTTON", L"Rotate 90", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 140, 660, 160, 28, hWnd, (HMENU)108, g_hInst, NULL);
		g_hCopy = CreateWindowW(L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 310, 660, 100, 28, hWnd, (HMENU)109, g_hInst, NULL);
		g_hDelete = CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 420, 660, 80, 28, hWnd, (HMENU)110, g_hInst, NULL);
		g_hChangeRoot = CreateWindowW(L"BUTTON", L"Change folder...", WS_CHILD | WS_VISIBLE | BS_NOTIFY, 250, 660, 120, 28, hWnd, (HMENU)111, g_hInst, NULL);

		g_hInfo = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | ES_MULTILINE | WS_VSCROLL | WS_BORDER | BS_NOTIFY, 520, 660, 260, 160, hWnd, NULL, g_hInst, NULL);

		UpdateInfoLabel();
		g_loading = true;
		StartBackground();
		InvalidateRect(g_hPanel, NULL, TRUE);
		return 0;
	}
	case WM_PAINT:
	{
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		RECT rc;
		GetClientRect(hWnd, &rc);
		FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
		EndPaint(hWnd, &ps);
	} break;
	case WM_COMMAND:
	{
		switch (LOWORD(wParam))
		{
		case 101: PrevImage(); break;
		case 102: NextImage(); break;
		case 103: // photoshop
		{
			DoOpenWith(L"C:\\Program Files\\Adobe\\Adobe Photoshop 2023\\Photoshop.exe");
		}
		break;

		case 104: // paint.net
		{
			DoOpenWith(L"C:\\Program Files\\paint.net\\PaintDotNet.exe");
		}
		break;
		case 105: // explorer
		{
			OpenInExplorer();
		}
		break;
		case 106: // toggle 100%
		{
			g_zoom = (g_zoom + 1) % 3;
			DWORD v = g_zoom;
			RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ImageViewer", L"Zoom100", REG_DWORD, &v, sizeof(DWORD));
			SetWindowTextW(g_hToggle100, g_zoom == 0 ? L"100%" : (g_zoom == 1 ? L"Fit" : L"Shrink"));
			InvalidateRect(g_hPanel, NULL, TRUE);
			break;
		}
		case 107: // toggle recursive
		{
			g_recursive = !g_recursive;
			DWORD v = g_recursive;
			RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ImageViewer", L"Recursive", REG_DWORD, &v, sizeof(DWORD));
			SetWindowTextW(g_hToggleRec, g_recursive ? L"Recursive: On" : L"Recursive: Off");
			EnumFiles(g_rootPath[0] ? g_rootPath : fs::current_path().wstring(), g_recursive);
			UpdateInfoLabel();
			InvalidateRect(g_hPanel, NULL, TRUE);
			break;
		}
		case 108: // rotate + resave exif
			Rotate90AndResave(true);
			EnumFiles(g_rootPath[0] ? g_rootPath : fs::current_path().wstring(), g_recursive);
			UpdateInfoLabel();
			InvalidateRect(g_hPanel, NULL, TRUE);
			break;
		case 109: // copy gif
		{
			std::lock_guard<std::mutex> lk(g_filesMutex);
			if (!g_files.empty()) CopyToClipboardAsGif(g_files[g_index]);
		}
		break;
		case 110: // delete
			DeleteCurrent();
			UpdateInfoLabel();
			InvalidateRect(g_hPanel, NULL, TRUE);
			break;
		case 111: // change root
			ChooseRootDirectory();
			EnumFiles(g_rootPath[0] ? g_rootPath : fs::current_path().wstring(), g_recursive);
			UpdateInfoLabel();
			InvalidateRect(g_hPanel, NULL, TRUE);
			break;
		}
		return 0;
	}
	case WM_TIMER:
		if (wParam == g_gifTimerId)
		{
			QueueNextFrame();
		}
		break;
	case WM_KEYDOWN:
		switch (wParam)
		{
		case VK_LEFT:
		case VK_PRIOR: // Page Up
			PrevImage();
			break;
		case VK_RIGHT:
		case VK_NEXT: // Page Down
			NextImage();
			break;
		case VK_OEM_COMMA: Rotate90AndResave(false); break; // '<' rotate left
		case VK_OEM_PERIOD: Rotate90AndResave(true); break;  // '>' rotate right (clockwise)
		}
		break;

	case WM_SIZE:
	{
		RECT r; GetClientRect(hWnd, &r);
		MoveWindow(g_hPanel, 10, 10, r.right - 20, r.bottom - 220, TRUE);
		MoveWindow(g_hPrev, 10, r.bottom - 200, 80, 28, TRUE);
		MoveWindow(g_hNext, 100, r.bottom - 200, 80, 28, TRUE);
		MoveWindow(g_hOpenPS, 200, r.bottom - 200, 160, 28, TRUE);
		MoveWindow(g_hOpenPN, 370, r.bottom - 200, 160, 28, TRUE);
		MoveWindow(g_hShowInExplorer, 540, r.bottom - 200, 130, 28, TRUE);
		MoveWindow(g_hToggle100, 680, r.bottom - 200, 100, 28, TRUE);
		MoveWindow(g_hToggleRec, 10, r.bottom - 160, 120, 28, TRUE);
		MoveWindow(g_hRotate, 140, r.bottom - 160, 160, 28, TRUE);
		MoveWindow(g_hCopy, 310, r.bottom - 160, 100, 28, TRUE);
		MoveWindow(g_hDelete, 420, r.bottom - 160, 80, 28, TRUE);
		MoveWindow(g_hInfo, 520, r.bottom - 160, r.right - 540, 150, TRUE);
		MoveWindow(g_hChangeRoot, 250, r.bottom - 120, 120, 28, TRUE);
		InvalidateRect(g_hPanel, NULL, TRUE);
		return 0;
	}
	case WM_DESTROY:
		g_loading = false;
		StopBackground();
		PostQuitMessage(0);
		return 0;
	}

	auto r = DefWindowProcW(hWnd, msg, wParam, lParam);
	SetFocus(hWnd);
	return r;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
	g_hInst = hInstance;
	// GDI+
	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
	INITCOMMONCONTROLSEX icce = { sizeof(icce), ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&icce);

	// initial root path -> current directory
	std::wstring cur = fs::current_path().wstring();
	wcsncpy_s(g_rootPath, cur.c_str(), _TRUNCATE);

	{
		DWORD type = REG_SZ;
		wchar_t buf[MAX_PATH];
		DWORD size = sizeof(buf);
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ImageViewer", L"RootPath", RRF_RT_REG_SZ, &type, buf, &size) == ERROR_SUCCESS) {
			wcsncpy_s(g_rootPath, buf, _TRUNCATE);
			g_index = 0;
		}

		DWORD val = 0;
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ImageViewer", L"Recursive", RRF_RT_REG_DWORD, nullptr, &val, &size) == ERROR_SUCCESS)
			g_recursive = val != 0;
		if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ImageViewer", L"Zoom100", RRF_RT_REG_DWORD, nullptr, &val, &size) == ERROR_SUCCESS)
			g_zoom = val;
	}

	// handle command-line arg: accept a single path
	int argc = 0;
	PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv)
	{
		if (argc > 1)
		{
			fs::path p(argv[1]);
			if (fs::exists(p))
			{
				if (fs::is_regular_file(p))
				{
					// show this file and set root dir to its parent
					wcsncpy_s(g_rootPath, p.parent_path().wstring().c_str(), _TRUNCATE);
					// find index
					for (size_t i = 0; i < g_files.size(); ++i) if (g_files[i] == p) { g_index = (int)i; break; }
				}
				else if (fs::is_directory(p))
				{
					wcsncpy_s(g_rootPath, p.wstring().c_str(), _TRUNCATE);
					g_index = 0;
				}
			}
		}
		LocalFree(argv);
	}

	if (wcslen(g_rootPath))
	{
		EnumFiles(g_rootPath, g_recursive);
	}

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = L"MinimalImgViewerClass";
	RegisterClassExW(&wc);

	g_hMain = CreateWindowExW(0, wc.lpszClassName, L"Minimal Image Viewer", WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, 1000, 820, NULL, NULL, hInstance, NULL);

	ShowWindow(g_hMain, nCmdShow);
	UpdateWindow(g_hMain);

	// start loader std::thread
	g_loading = true;
	StartBackground();

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	{
		g_backBuffer.reset();

		std::lock_guard<std::mutex> lk(g_cacheMutex);
		g_cache.clear(); // destroys all shared_ptr<Bitmap> while GDI+ is still alive
	}
	GdiplusShutdown(g_gdiplusToken);
	return 0;
}
