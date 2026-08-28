#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "resource.h"
#include "MBR.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

void RunBSOD();
int HookClassMain();
int LimitMain();

namespace
{
HWND g_systemTaskbarWindow = nullptr;
HDC g_backBufferDc = nullptr;
HBITMAP g_backBufferBitmap = nullptr;
HGDIOBJ g_backBufferOriginalBitmap = nullptr;
INT g_backBufferWidth = 0;
INT g_backBufferHeight = 0;

HDC EnsureBackBuffer(HDC screenDc, INT width, INT height)
{
    if (width <= 0 || height <= 0)
        return nullptr;
    if (!g_backBufferDc)
        g_backBufferDc = CreateCompatibleDC(screenDc);
    if (!g_backBufferDc)
        return nullptr;
    if (g_backBufferBitmap && width == g_backBufferWidth && height == g_backBufferHeight)
        return g_backBufferDc;

    HBITMAP nextBitmap = CreateCompatibleBitmap(screenDc, width, height);
    if (!nextBitmap)
        return nullptr;
    HGDIOBJ previous = SelectObject(g_backBufferDc, nextBitmap);
    if (!g_backBufferOriginalBitmap)
        g_backBufferOriginalBitmap = previous;
    else if (g_backBufferBitmap)
        DeleteObject(g_backBufferBitmap);
    g_backBufferBitmap = nextBitmap;
    g_backBufferWidth = width;
    g_backBufferHeight = height;
    return g_backBufferDc;
}

void ReleaseBackBuffer()
{
    if (g_backBufferDc && g_backBufferOriginalBitmap)
        SelectObject(g_backBufferDc, g_backBufferOriginalBitmap);
    if (g_backBufferBitmap)
        DeleteObject(g_backBufferBitmap);
    if (g_backBufferDc)
        DeleteDC(g_backBufferDc);
    g_backBufferDc = nullptr;
    g_backBufferBitmap = nullptr;
    g_backBufferOriginalBitmap = nullptr;
    g_backBufferWidth = 0;
    g_backBufferHeight = 0;
}

void HideSystemTaskbar()
{
    g_systemTaskbarWindow = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (g_systemTaskbarWindow)
        ShowWindow(g_systemTaskbarWindow, SW_HIDE);
}

void RestoreSystemTaskbar()
{
    if (g_systemTaskbarWindow && IsWindow(g_systemTaskbarWindow))
        ShowWindow(g_systemTaskbarWindow, SW_SHOW);
    g_systemTaskbarWindow = nullptr;
}

struct TaskbarRestoreGuard
{
    ~TaskbarRestoreGuard() { RestoreSystemTaskbar(); }
};

constexpr UINT_PTR kGifTimerId = 1;
constexpr ULONGLONG kForceTopmostIntervalMs = 1;
constexpr REAL kGifScale = 0.432f;
constexpr REAL kLoadGifScale = 0.66f;
constexpr INT kBoot1VerticalOffset = 40;
// 普通 Win32 定时器会按系统时钟粒度合并；10ms 请求通常得到约15.6ms（约64Hz）。
constexpr UINT kGifClockIntervalMs = 10;
constexpr ULONGLONG kSwitchTo99PercentAfterMs = 15000;
constexpr ULONGLONG kStageDurationMs = 5000;
constexpr ULONGLONG kLoadStageDurationMs = 20000;
constexpr ULONGLONG kBackground3DurationMs = 5000;
constexpr ULONGLONG kStaticBootImageDurationMs = 5000;
constexpr ULONGLONG kKb2FlashIntervalMs = 10000;
constexpr ULONGLONG kKb2FlashDurationMs = 500;
constexpr ULONGLONG kStartMenu2DelayMs = 20000;
constexpr ULONGLONG kGlitchFrameIntervalMs = 500;
constexpr ULONGLONG kGlitchDurationMs = 5000;
constexpr ULONGLONG kExplosionDurationMs = 2112;
constexpr ULONGLONG kPhotoSound1DurationMs = 9740;
constexpr ULONGLONG kPhotoPreviewDurationMs = 3000;
constexpr ULONGLONG kPhotoNullSoundDurationMs = 3297;
constexpr ULONGLONG kTextDisplayDurationMs = 5000;

enum ResourceId : WORD
{
    ResSetupBackground = 1001, ResSetup1 = 1002, ResSetup99 = 1003, ResSetupError = 1004,
    ResLoadBackground2 = 1010, ResLoadBackground3 = 1011, ResLoadGif = 1012, ResExplosion = 1013,
    ResBoot1 = 1020, ResBoot2 = 1021, ResBootSound = 1022,
    ResDesktop = 1030, ResTaskbar = 1031, ResStartMenu = 1032, ResStartMenu2 = 1033,
    ResComputer = 1034, ResComputerNull = 1035, ResRecycle = 1036, ResRecycleNull = 1037,
    ResTerror = 1038, ResPhotoIcon = 1039, ResPhotoIconNull = 1040,
    ResPhoto = 1041, ResPhotoNull = 1042, ResTextIcon = 1043, ResText1 = 1044,
    ResText2 = 1045, ResDesktopMusic = 1046, ResGlitchSound = 1047,
    ResPhotoSound1 = 1048, ResDoogSound = 1049, ResTextIconNull = 1050,
    ResFolderIcon = 1051, ResFolderIconNull = 1052, ResGoodbyeIcon = 1053,
    ResEndGif1 = 1171, ResEndGif2 = 1172, ResEndB11 = 1173, ResEndB12 = 1174,
    ResEndB21 = 1175, ResEndB2Null = 1176, ResRealness = 1177,
    ResEndError = 1178
};

enum class DesktopEffect
{
    None,
    ComputerGlitch,
    RecycleTerror,
    PhotoFirst,
    PhotoSecondPreview,
    PhotoNullSound,
    TextFirst,
    TextSecondPreview,
    TextKbChaos,
    FolderChaos
};

enum class DisplayStage
{
    OnePercent,
    NinetyNinePercent,
    Error,
    Loading,
    Finished,
    BootOne,
    BootTwo,
    BootComplete,
    Desktop
};

std::unique_ptr<Image> g_background;
std::unique_ptr<Image> g_background2;
std::unique_ptr<Image> g_background3;
std::unique_ptr<Image> g_progressGif;
std::unique_ptr<Image> g_composedGifFrame;
std::unique_ptr<Image> g_progress99Gif;
std::unique_ptr<Image> g_errorGif;
std::unique_ptr<Image> g_loadGif;
std::unique_ptr<Image> g_boot1Gif;
std::unique_ptr<Image> g_boot2Gif;
std::unique_ptr<Image> g_desktopImage;
std::unique_ptr<Image> g_taskbarImage;
std::unique_ptr<Image> g_startMenuImage;
std::unique_ptr<Image> g_startMenu2Image;
std::unique_ptr<Image> g_computerImage;
std::unique_ptr<Image> g_computerNullImage;
std::unique_ptr<Image> g_recycleImage;
std::unique_ptr<Image> g_recycleNullImage;
std::unique_ptr<Image> g_terrorImage;
std::unique_ptr<Image> g_photoIconImage;
std::unique_ptr<Image> g_photoIconNullImage;
std::unique_ptr<Image> g_photoImage;
std::unique_ptr<Image> g_photoNullImage;
std::unique_ptr<Image> g_textIconImage;
std::unique_ptr<Image> g_textIconNullImage;
std::unique_ptr<Image> g_folderIconImage;
std::unique_ptr<Image> g_folderIconNullImage;
std::unique_ptr<Image> g_goodbyeIconImage;
std::unique_ptr<Image> g_endGif1;
std::unique_ptr<Image> g_endGif2;
std::unique_ptr<Image> g_endB11;
std::unique_ptr<Image> g_endB12;
std::unique_ptr<Image> g_endB21;
std::unique_ptr<Image> g_endB2Null;
std::unique_ptr<Image> g_endErrorImage;
std::unique_ptr<Image> g_text1Image;
std::unique_ptr<Image> g_text2Image;
std::vector<std::unique_ptr<Image>> g_kb2Images;
std::vector<std::unique_ptr<Image>> g_kb1Images;
std::vector<std::unique_ptr<Image>> g_glitchImages;
struct StreamReleaser
{
    void operator()(IStream* stream) const
    {
        if (stream)
            stream->Release();
    }
};
std::unique_ptr<IStream, StreamReleaser> g_backgroundStream;
std::unique_ptr<IStream, StreamReleaser> g_background2Stream;
std::unique_ptr<IStream, StreamReleaser> g_background3Stream;
std::unique_ptr<IStream, StreamReleaser> g_progressGifStream;
std::unique_ptr<IStream, StreamReleaser> g_progress99GifStream;
std::unique_ptr<IStream, StreamReleaser> g_errorGifStream;
std::unique_ptr<IStream, StreamReleaser> g_loadGifStream;
std::unique_ptr<IStream, StreamReleaser> g_boot1GifStream;
std::unique_ptr<IStream, StreamReleaser> g_boot2GifStream;
std::vector<std::unique_ptr<IStream, StreamReleaser>> g_managedImageStreams;
std::vector<UINT> g_frameDelays;
UINT g_frameCount = 0;
UINT g_currentFrame = 0;
ULONGLONG g_lastFrameTick = 0;
ULONGLONG g_stageStartTick = 0;
ULONGLONG g_nextForceTopmostTick = 0;
DisplayStage g_stage = DisplayStage::OnePercent;
bool g_loopGif = true;
bool g_gifCompleted = false;
bool g_startMenuVisible = false;
bool g_showStartMenu2 = false;
bool g_kb2FlashVisible = false;
size_t g_kb2FlashIndex = 0;
ULONGLONG g_desktopStartTick = 0;
ULONGLONG g_nextKb2FlashTick = 0;
ULONGLONG g_kb2FlashEndTick = 0;
DesktopEffect g_desktopEffect = DesktopEffect::None;
ULONGLONG g_desktopEffectEndTick = 0;
ULONGLONG g_nextGlitchFrameTick = 0;
size_t g_glitchImageIndex = 0;
bool g_computerDestroyed = false;
bool g_recycleDestroyed = false;
UINT g_photoClickCount = 0;
bool g_photoDestroyed = false;
UINT g_textClickCount = 0;
bool g_textCompleted = false;
size_t g_textChaosImageIndex = 0;
bool g_folderDestroyed = false;
size_t g_folderChaosImageIndex = 0;
bool g_endSequenceActive = false;
bool g_endSecondState = false;
bool g_endErrorVisible = false;
ULONGLONG g_endErrorEndTick = 0;
std::vector<WORD> g_kbMusicIds;
const UINT g_kbMusicDurationsMs[] = { 1022, 4389, 2978, 14739, 1361, 2038 };

std::wstring GetExecutableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring directory(path);
    const size_t separator = directory.find_last_of(L"\\/");
    return separator == std::wstring::npos ? L"." : directory.substr(0, separator);
}

std::unique_ptr<Image> LoadImageFile(const std::wstring& path)
{
    auto image = std::unique_ptr<Image>(Image::FromFile(path.c_str(), FALSE));
    return image && image->GetLastStatus() == Ok ? std::move(image) : nullptr;
}

std::unique_ptr<Image> LoadImageResource(
    HINSTANCE instance,
    WORD resourceId,
    LPCWSTR resourceType,
    std::unique_ptr<IStream, StreamReleaser>& resourceStream)
{
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), resourceType);
    if (!resource)
        return nullptr;

    const DWORD resourceSize = SizeofResource(instance, resource);
    const HGLOBAL loadedResource = LoadResource(instance, resource);
    const void* resourceData = LockResource(loadedResource);
    if (!resourceSize || !resourceData)
        return nullptr;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (!memory)
        return nullptr;

    void* memoryData = GlobalLock(memory);
    if (!memoryData)
    {
        GlobalFree(memory);
        return nullptr;
    }
    CopyMemory(memoryData, resourceData, resourceSize);
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)))
    {
        GlobalFree(memory);
        return nullptr;
    }

    resourceStream.reset(stream); // GDI+ Image 在其生命周期内必须保持流有效。
    return std::unique_ptr<Image>(Image::FromStream(stream, FALSE));
}

std::unique_ptr<Image> LoadManagedImageResource(HINSTANCE instance, WORD resourceId)
{
    std::unique_ptr<IStream, StreamReleaser> stream;
    auto image = LoadImageResource(instance, resourceId, RT_RCDATA, stream);
    if (stream)
        g_managedImageStreams.push_back(std::move(stream));
    return image;
}

void SelectGifFrame(UINT frame)
{
    if (!g_progressGif || frame >= g_frameCount)
        return;

    const GUID dimension = FrameDimensionTime;
    g_progressGif->SelectActiveFrame(&dimension, frame);

    // 复用同一张合成位图。旧实现每帧 Clone 一张1366x768位图（约4MB），
    // 会造成高频内存分配、复制和回收，从而出现明显卡顿。
    REAL renderScale = 1.0f;
    if (g_progressGif->GetWidth() >= 1000)
        renderScale = kGifScale;
    else if (g_progressGif->GetWidth() == 220)
        renderScale = kLoadGifScale;
    const UINT renderWidth = max(1U,
        static_cast<UINT>(g_progressGif->GetWidth() * renderScale));
    const UINT renderHeight = max(1U,
        static_cast<UINT>(g_progressGif->GetHeight() * renderScale));
    if (!g_composedGifFrame ||
        g_composedGifFrame->GetWidth() != renderWidth ||
        g_composedGifFrame->GetHeight() != renderHeight)
    {
        g_composedGifFrame = std::make_unique<Bitmap>(
            renderWidth, renderHeight, PixelFormat32bppPARGB);
    }
    Graphics frameGraphics(g_composedGifFrame.get());
    frameGraphics.SetCompositingMode(CompositingModeSourceCopy);
    frameGraphics.Clear(Color::Transparent);
    frameGraphics.DrawImage(g_progressGif.get(), 0, 0,
        static_cast<INT>(renderWidth), static_cast<INT>(renderHeight));
}

void InvalidateGifRegion(HWND window)
{
    if (!g_progressGif)
        return;
    RECT client{};
    GetClientRect(window, &client);
    RECT dirty{};
    if (g_endSequenceActive)
    {
        dirty.left = (client.right - 500) / 2;
        dirty.top = (client.bottom - (247 + 38 * 2)) / 2;
        dirty.right = dirty.left + 500;
        dirty.bottom = dirty.top + 247;
    }
    else if (g_stage == DisplayStage::Loading ||
        g_stage == DisplayStage::BootOne || g_stage == DisplayStage::BootTwo ||
        g_stage == DisplayStage::BootComplete)
    {
        const REAL scale = g_stage == DisplayStage::Loading ? kLoadGifScale : 1.0f;
        const INT width = static_cast<INT>(g_progressGif->GetWidth() * scale);
        const INT height = static_cast<INT>(g_progressGif->GetHeight() * scale);
        dirty.left = (client.right - width) / 2;
        dirty.top = (client.bottom - height) / 2 +
            (g_stage == DisplayStage::BootOne ? kBoot1VerticalOffset : 0);
        dirty.right = dirty.left + width;
        dirty.bottom = dirty.top + height;
    }
    else
    {
        dirty.left = 0;
        dirty.top = 0;
        dirty.right = static_cast<LONG>(g_progressGif->GetWidth() * kGifScale);
        dirty.bottom = static_cast<LONG>(g_progressGif->GetHeight() * kGifScale);
    }
    InflateRect(&dirty, 2, 2);
    InvalidateRect(window, &dirty, FALSE);
}

void LoadGifDelays()
{
    g_frameDelays.assign(g_frameCount, 100U);

    const UINT propertySize = g_progressGif->GetPropertyItemSize(PropertyTagFrameDelay);
    if (propertySize == 0)
        return;

    std::vector<BYTE> propertyBuffer(propertySize);
    auto* property = reinterpret_cast<PropertyItem*>(propertyBuffer.data());
    if (g_progressGif->GetPropertyItem(PropertyTagFrameDelay, propertySize, property) != Ok)
        return;

    const auto* delays = static_cast<const UINT*>(property->value);
    const UINT count = property->length / sizeof(UINT);
    const UINT usableCount = min(count, g_frameCount);
    for (UINT index = 0; index < usableCount; ++index)
    {
        // GIF 延迟的单位是 1/100 秒；0 在不少编码器中表示“使用默认值”。
        g_frameDelays[index] = delays[index] == 0 ? 100U : max(10U, delays[index] * 10U);
    }
}

UINT GetCurrentFrameDelayMs()
{
    return g_currentFrame < g_frameDelays.size() ? g_frameDelays[g_currentFrame] : 100U;
}

bool AdvanceGifFrames()
{
    const ULONGLONG now = GetTickCount64();
    ULONGLONG elapsed = now - g_lastFrameTick;
    bool frameChanged = false;

    // 计时器消息若被延后，按实际经过时间补帧，防止动画逐渐变慢。
    while (elapsed >= GetCurrentFrameDelayMs())
    {
        elapsed -= GetCurrentFrameDelayMs();
        if (!g_loopGif && g_currentFrame + 1 >= g_frameCount)
        {
            g_gifCompleted = true;
            break;
        }
        g_currentFrame = (g_currentFrame + 1) % g_frameCount;
        frameChanged = true;
    }

    if (frameChanged)
    {
        g_lastFrameTick = now - elapsed;
        SelectGifFrame(g_currentFrame);
    }

    return frameChanged;
}

void InitializeGifAnimation()
{
    g_frameCount = 0;
    g_frameDelays.clear();
    g_currentFrame = 0;
    g_composedGifFrame.reset();
    g_gifCompleted = false;

    if (!g_progressGif || g_progressGif->GetLastStatus() != Ok)
        return;

    const GUID dimension = FrameDimensionTime;
    g_frameCount = g_progressGif->GetFrameCount(&dimension);
    if (g_frameCount == 0)
    {
        // 有些扩展名为 GIF 的资源实际只有一个静态帧，没有 Time 维度。
        // 仍将其视为一帧，保证画面显示且后续阶段计时继续运行。
        g_frameCount = 1;
        g_frameDelays.assign(1, 100U);
        g_lastFrameTick = GetTickCount64();
        return;
    }

    LoadGifDelays();
    SelectGifFrame(0);
    g_lastFrameTick = GetTickCount64();
}

void SwapActiveGif(
    std::unique_ptr<Image>& nextGif,
    std::unique_ptr<IStream, StreamReleaser>& nextStream)
{
    g_progressGif.swap(nextGif);
    g_progressGifStream.swap(nextStream);
    InitializeGifAnimation();
}

void PlayEmbeddedWave(HINSTANCE instance, WORD resourceId, bool loop = false)
{
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource)
        return;

    const HGLOBAL loadedResource = LoadResource(instance, resource);
    const void* resourceData = loadedResource ? LockResource(loadedResource) : nullptr;
    if (resourceData)
    {
        DWORD flags = SND_MEMORY | SND_ASYNC | SND_NODEFAULT;
        if (loop)
            flags |= SND_LOOP;
        PlaySoundW(reinterpret_cast<LPCWSTR>(resourceData), nullptr, flags);
    }
}

void DrawImageCentered(
    Graphics& graphics, Image* image, INT clientWidth, INT clientHeight,
    REAL maximumScale = 1.0f)
{
    if (!image || image->GetWidth() == 0 || image->GetHeight() == 0)
        return;

    // 保持原始宽高比；只有图片超出屏幕时才等比缩小，避免裁切后产生视觉偏移。
    const REAL scale = min(maximumScale, min(
        static_cast<REAL>(clientWidth) / image->GetWidth(),
        static_cast<REAL>(clientHeight) / image->GetHeight()));
    const INT width = max(1, static_cast<INT>(image->GetWidth() * scale));
    const INT height = max(1, static_cast<INT>(image->GetHeight() * scale));
    const INT x = (clientWidth - width) / 2;
    const INT y = (clientHeight - height) / 2;
    graphics.DrawImage(image, x, y, width, height);
}
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        SetCursor(nullptr);
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_nextForceTopmostTick = GetTickCount64() + kForceTopmostIntervalMs;
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        g_background = LoadManagedImageResource(instance, ResSetupBackground);
        g_progressGif = LoadManagedImageResource(instance, ResSetup1);
        g_progress99Gif = LoadManagedImageResource(instance, ResSetup99);
        g_errorGif = LoadManagedImageResource(instance, ResSetupError);
        g_background2 = LoadManagedImageResource(instance, ResLoadBackground2);
        g_background3 = LoadManagedImageResource(instance, ResLoadBackground3);
        g_loadGif = LoadManagedImageResource(instance, ResLoadGif);
        g_boot1Gif = LoadManagedImageResource(instance, ResBoot1);
        g_boot2Gif = LoadManagedImageResource(instance, ResBoot2);
        g_desktopImage = LoadManagedImageResource(instance, ResDesktop);
        g_taskbarImage = LoadManagedImageResource(instance, ResTaskbar);
        g_startMenuImage = LoadManagedImageResource(instance, ResStartMenu);
        g_startMenu2Image = LoadManagedImageResource(instance, ResStartMenu2);
        g_computerImage = LoadManagedImageResource(instance, ResComputer);
        g_computerNullImage = LoadManagedImageResource(instance, ResComputerNull);
        g_recycleImage = LoadManagedImageResource(instance, ResRecycle);
        g_recycleNullImage = LoadManagedImageResource(instance, ResRecycleNull);
        g_terrorImage = LoadManagedImageResource(instance, ResTerror);
        g_photoIconImage = LoadManagedImageResource(instance, ResPhotoIcon);
        g_photoIconNullImage = LoadManagedImageResource(instance, ResPhotoIconNull);
        g_photoImage = LoadManagedImageResource(instance, ResPhoto);
        g_photoNullImage = LoadManagedImageResource(instance, ResPhotoNull);
        g_textIconImage = LoadManagedImageResource(instance, ResTextIcon);
        g_textIconNullImage = LoadManagedImageResource(instance, ResTextIconNull);
        g_folderIconImage = LoadManagedImageResource(instance, ResFolderIcon);
        g_folderIconNullImage = LoadManagedImageResource(instance, ResFolderIconNull);
        g_goodbyeIconImage = LoadManagedImageResource(instance, ResGoodbyeIcon);
        g_endGif1 = LoadManagedImageResource(instance, ResEndGif1);
        g_endGif2 = LoadManagedImageResource(instance, ResEndGif2);
        g_endB11 = LoadManagedImageResource(instance, ResEndB11);
        g_endB12 = LoadManagedImageResource(instance, ResEndB12);
        g_endB21 = LoadManagedImageResource(instance, ResEndB21);
        g_endB2Null = LoadManagedImageResource(instance, ResEndB2Null);
        g_endErrorImage = LoadManagedImageResource(instance, ResEndError);
        g_text1Image = LoadManagedImageResource(instance, ResText1);
        g_text2Image = LoadManagedImageResource(instance, ResText2);
        for (WORD id = 1101; id <= 1106; ++id)
        {
            auto image = LoadManagedImageResource(instance, id);
            if (image) g_kb2Images.push_back(std::move(image));
        }
        for (WORD id = 1161; id <= 1168; ++id)
        {
            auto image = LoadManagedImageResource(instance, id);
            if (image) g_kb1Images.push_back(std::move(image));
        }
        for (WORD id = 1121; id <= 1126; ++id)
        {
            auto image = LoadManagedImageResource(instance, id);
            if (image) g_glitchImages.push_back(std::move(image));
        }
        for (WORD id = 1141; id <= 1146; ++id)
            g_kbMusicIds.push_back(id);

        // 嵌入资源无法加载时给出具体原因，避免只显示黑屏。
        if (!g_background || g_background->GetLastStatus() != Ok ||
            !g_background2 || g_background2->GetLastStatus() != Ok ||
            !g_background3 || g_background3->GetLastStatus() != Ok ||
            !g_progressGif || g_progressGif->GetLastStatus() != Ok ||
            !g_progress99Gif || g_progress99Gif->GetLastStatus() != Ok ||
            !g_errorGif || g_errorGif->GetLastStatus() != Ok ||
            !g_loadGif || g_loadGif->GetLastStatus() != Ok ||
            !g_boot1Gif || g_boot1Gif->GetLastStatus() != Ok ||
            !g_boot2Gif || g_boot2Gif->GetLastStatus() != Ok ||
            !g_desktopImage || !g_taskbarImage || !g_startMenuImage ||
            !g_startMenu2Image || !g_computerImage || !g_computerNullImage ||
            !g_recycleImage || !g_recycleNullImage || !g_terrorImage ||
            !g_photoIconImage || !g_photoIconNullImage ||
            !g_photoImage || !g_photoNullImage ||
            !g_textIconImage || !g_textIconNullImage ||
            !g_folderIconImage || !g_folderIconNullImage ||
            !g_goodbyeIconImage || !g_endGif1 || !g_endGif2 ||
            !g_endB11 || !g_endB12 || !g_endB21 || !g_endB2Null ||
            !g_endErrorImage ||
            !g_text1Image || !g_text2Image ||
            g_kb2Images.empty() || g_kb1Images.empty() || g_glitchImages.empty())
        {
            MessageBoxW(window, L"无法加载内嵌的 Setup-Load 图片或 GIF 资源。",
                L"资源加载失败", MB_ICONERROR | MB_OK);
        }

        if (g_progressGif && g_progressGif->GetLastStatus() == Ok)
        {
            InitializeGifAnimation();
            if (g_frameCount > 0)
            {
                g_stageStartTick = GetTickCount64();
                SetTimer(window, kGifTimerId, kGifClockIntervalMs, nullptr);
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == kGifTimerId)
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= g_nextForceTopmostTick)
            {
                SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                g_nextForceTopmostTick = now + kForceTopmostIntervalMs;
            }
            const ULONGLONG stageElapsed = now - g_stageStartTick;
            bool stageChanged = false;

            if (g_stage == DisplayStage::OnePercent &&
                stageElapsed >= kSwitchTo99PercentAfterMs)
            {
                SwapActiveGif(g_progress99Gif, g_progress99GifStream);
                g_stage = DisplayStage::NinetyNinePercent;
                stageChanged = true;
            }
            else if (g_stage == DisplayStage::NinetyNinePercent &&
                stageElapsed >= kStageDurationMs)
            {
                SwapActiveGif(g_errorGif, g_errorGifStream);
                g_stage = DisplayStage::Error;
                stageChanged = true;
            }
            else if (g_stage == DisplayStage::Error &&
                stageElapsed >= kStageDurationMs)
            {
                g_background.swap(g_background2);
                g_backgroundStream.swap(g_background2Stream);
                SwapActiveGif(g_loadGif, g_loadGifStream);
                g_stage = DisplayStage::Loading;
                stageChanged = true;
            }
            else if (g_stage == DisplayStage::Loading &&
                stageElapsed >= kLoadStageDurationMs)
            {
                g_background.swap(g_background3);
                g_backgroundStream.swap(g_background3Stream);
                g_progressGif.reset();
                g_progressGifStream.reset();
                g_frameCount = 0;
                g_frameDelays.clear();
                g_stage = DisplayStage::Finished;
                PlayEmbeddedWave(GetModuleHandleW(nullptr), ResExplosion);
                stageChanged = true;
            }
            else if (g_stage == DisplayStage::Finished &&
                stageElapsed >= kBackground3DurationMs)
            {
                g_background.swap(g_background3);
                g_backgroundStream.swap(g_background3Stream);
                SwapActiveGif(g_boot1Gif, g_boot1GifStream);
                g_loopGif = false;
                g_stage = DisplayStage::BootOne;
                PlayEmbeddedWave(GetModuleHandleW(nullptr), ResBootSound, true);
                stageChanged = true;
            }
            else if (g_stage == DisplayStage::BootOne &&
                (g_gifCompleted ||
                    (g_frameCount <= 1 && stageElapsed >= kStaticBootImageDurationMs)))
            {
                SwapActiveGif(g_boot2Gif, g_boot2GifStream);
                g_loopGif = false;
                g_stage = DisplayStage::BootTwo;
                stageChanged = true;
            }
            else if (g_stage == DisplayStage::BootTwo &&
                (g_gifCompleted ||
                    (g_frameCount <= 1 && stageElapsed >= kStaticBootImageDurationMs)))
            {
                PlaySoundW(nullptr, nullptr, 0);
                g_progressGif.reset();
                g_progressGifStream.reset();
                g_frameCount = 0;
                g_frameDelays.clear();
                g_stage = DisplayStage::Desktop;
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                g_desktopStartTick = now;
                g_nextKb2FlashTick = now + kKb2FlashIntervalMs;
                g_kb2FlashVisible = false;
                PlayEmbeddedWave(GetModuleHandleW(nullptr), ResDesktopMusic, true);
                stageChanged = true;
            }
            else if (g_stage == DisplayStage::Desktop)
            {
                if (g_endErrorVisible && now >= g_endErrorEndTick)
                {
                    g_endErrorVisible = false;
                    InvalidateRect(window, nullptr, FALSE);
                    RunBSOD();
                    return 0;
                }
                if (g_desktopEffect != DesktopEffect::None &&
                    now >= g_desktopEffectEndTick)
                {
                    if (g_desktopEffect == DesktopEffect::PhotoSecondPreview)
                    {
                        g_desktopEffect = DesktopEffect::PhotoNullSound;
                        g_desktopEffectEndTick = now + kPhotoNullSoundDurationMs;
                        PlayEmbeddedWave(GetModuleHandleW(nullptr), ResDoogSound);
                        InvalidateRect(window, nullptr, FALSE);
                        return 0;
                    }
                    if (g_desktopEffect == DesktopEffect::TextSecondPreview)
                    {
                        const size_t musicIndex = static_cast<size_t>(
                            (now ^ (now >> 7)) % g_kbMusicIds.size());
                        g_desktopEffect = DesktopEffect::TextKbChaos;
                        g_desktopEffectEndTick = now + g_kbMusicDurationsMs[musicIndex];
                        g_textChaosImageIndex = static_cast<size_t>(
                            (now ^ (now >> 9) ^ GetCurrentThreadId()) % g_kb1Images.size());
                        PlayEmbeddedWave(GetModuleHandleW(nullptr), g_kbMusicIds[musicIndex]);
                        InvalidateRect(window, nullptr, FALSE);
                        return 0;
                    }

                    if (g_desktopEffect == DesktopEffect::ComputerGlitch)
                        g_computerDestroyed = true;
                    else if (g_desktopEffect == DesktopEffect::RecycleTerror)
                        g_recycleDestroyed = true;
                    else if (g_desktopEffect == DesktopEffect::PhotoFirst)
                        g_photoClickCount = 1;
                    else if (g_desktopEffect == DesktopEffect::PhotoNullSound)
                        g_photoDestroyed = true;
                    else if (g_desktopEffect == DesktopEffect::TextFirst)
                        g_textClickCount = 1;
                    else if (g_desktopEffect == DesktopEffect::TextKbChaos)
                        g_textCompleted = true;
                    else if (g_desktopEffect == DesktopEffect::FolderChaos)
                        g_folderDestroyed = true;

                    g_desktopEffect = DesktopEffect::None;
                    g_nextKb2FlashTick = now + kKb2FlashIntervalMs;
                    PlayEmbeddedWave(GetModuleHandleW(nullptr), ResDesktopMusic, true);
                    InvalidateRect(window, nullptr, FALSE);
                }
                else if (g_desktopEffect == DesktopEffect::ComputerGlitch &&
                    now >= g_nextGlitchFrameTick && !g_glitchImages.empty())
                {
                    const size_t previousIndex = g_glitchImageIndex;
                    g_glitchImageIndex = static_cast<size_t>(
                        (now ^ (now >> 9) ^ GetCurrentThreadId()) % g_glitchImages.size());
                    if (g_glitchImages.size() > 1 && g_glitchImageIndex == previousIndex)
                        g_glitchImageIndex = (g_glitchImageIndex + 1) % g_glitchImages.size();
                    g_nextGlitchFrameTick = now + kGlitchFrameIntervalMs;
                    InvalidateRect(window, nullptr, FALSE);
                }
                else if (g_desktopEffect == DesktopEffect::None &&
                    g_kb2FlashVisible && now >= g_kb2FlashEndTick)
                {
                    g_kb2FlashVisible = false;
                    InvalidateRect(window, nullptr, FALSE);
                }
                else if (!g_endSequenceActive &&
                    g_desktopEffect == DesktopEffect::None &&
                    !g_kb2FlashVisible && now >= g_nextKb2FlashTick &&
                    !g_kb2Images.empty())
                {
                    const size_t previousIndex = g_kb2FlashIndex;
                    g_kb2FlashIndex = static_cast<size_t>(
                        (now ^ (now >> 11) ^ GetCurrentThreadId()) % g_kb2Images.size());
                    if (g_kb2Images.size() > 1 && g_kb2FlashIndex == previousIndex)
                        g_kb2FlashIndex = (g_kb2FlashIndex + 1) % g_kb2Images.size();
                    g_kb2FlashVisible = true;
                    g_kb2FlashEndTick = now + kKb2FlashDurationMs;
                    g_nextKb2FlashTick = now + kKb2FlashIntervalMs;
                    InvalidateRect(window, nullptr, FALSE);
                }
            }

            if (stageChanged)
            {
                g_stageStartTick = now;
                InvalidateRect(window, nullptr, FALSE);
            }

            if (g_frameCount > 0 && AdvanceGifFrames())
                InvalidateGifRegion(window);
        }
        return 0;

    case WM_ERASEBKGND:
        // WM_PAINT 会通过双缓冲完整绘制窗口，禁止系统先清屏以避免闪烁。
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC hdc = BeginPaint(window, &paint);

        RECT client{};
        GetClientRect(window, &client);

        // 复用持久化全屏缓冲区，避免每个 GIF 帧反复分配和销毁大型 GDI 位图。
        HDC bufferDc = EnsureBackBuffer(hdc, client.right, client.bottom);
        if (!bufferDc)
        {
            EndPaint(window, &paint);
            return 0;
        }
        {
            Graphics graphics(bufferDc);
            const Rect dirtyRect(
                paint.rcPaint.left, paint.rcPaint.top,
                paint.rcPaint.right - paint.rcPaint.left,
                paint.rcPaint.bottom - paint.rcPaint.top);
            graphics.SetClip(dirtyRect, CombineModeReplace);
            graphics.SetCompositingQuality(CompositingQualityHighSpeed);
            graphics.SetInterpolationMode(InterpolationModeBilinear);
            graphics.SetPixelOffsetMode(PixelOffsetModeHighSpeed);
            graphics.Clear(Color::Black);

            if (g_stage == DisplayStage::Desktop && g_desktopImage && g_taskbarImage)
            {
                // 保持桌面原始宽高比缩放，避免全屏适配时产生变形。
                const REAL desktopScale = min(
                    static_cast<REAL>(client.right) / g_desktopImage->GetWidth(),
                    static_cast<REAL>(client.bottom) / g_desktopImage->GetHeight());
                const INT desktopWidth = static_cast<INT>(g_desktopImage->GetWidth() * desktopScale);
                const INT desktopHeight = static_cast<INT>(g_desktopImage->GetHeight() * desktopScale);
                const INT desktopX = (client.right - desktopWidth) / 2;
                const INT desktopY = (client.bottom - desktopHeight) / 2;
                graphics.DrawImage(g_desktopImage.get(), desktopX, desktopY, desktopWidth, desktopHeight);

                Image* computerIcon = g_computerDestroyed
                    ? g_computerNullImage.get() : g_computerImage.get();
                Image* recycleIcon = g_recycleDestroyed
                    ? g_recycleNullImage.get() : g_recycleImage.get();
                Image* photoIcon = g_photoDestroyed
                    ? g_photoIconNullImage.get() : g_photoIconImage.get();
                if (computerIcon)
                    graphics.DrawImage(computerIcon, 20, 20, 100, 100);
                if (recycleIcon)
                    graphics.DrawImage(recycleIcon, 20, 130, 100, 100);
                if (photoIcon)
                    graphics.DrawImage(photoIcon, 20, 240, 100, 100);
                Image* textIcon = g_textCompleted
                    ? g_textIconNullImage.get() : g_textIconImage.get();
                if (textIcon)
                    graphics.DrawImage(textIcon, 20, 350, 100, 100);
                Image* folderIcon = g_folderDestroyed
                    ? g_folderIconNullImage.get() : g_folderIconImage.get();
                if (folderIcon)
                    graphics.DrawImage(folderIcon, 20, 460, 100, 100);
                if (g_goodbyeIconImage)
                    graphics.DrawImage(g_goodbyeIconImage.get(), 20, 570, 100, 100);

                const INT taskbarHeight = max(1, static_cast<INT>(
                    g_taskbarImage->GetHeight() * static_cast<REAL>(client.right) /
                    g_taskbarImage->GetWidth()));
                const INT taskbarY = client.bottom - taskbarHeight;

                Image* startMenu = g_showStartMenu2
                    ? g_startMenu2Image.get()
                    : g_startMenuImage.get();
                if (g_startMenuVisible && startMenu)
                {
                    const INT availableHeight = max(1, taskbarY);
                    const REAL menuScale = min(1.0f, min(
                        static_cast<REAL>(client.right) / startMenu->GetWidth(),
                        static_cast<REAL>(availableHeight) / startMenu->GetHeight()));
                    const INT menuWidth = static_cast<INT>(startMenu->GetWidth() * menuScale);
                    const INT menuHeight = static_cast<INT>(startMenu->GetHeight() * menuScale);
                    const INT menuX = (client.right - menuWidth) / 2;
                    const INT menuY = taskbarY - menuHeight;
                    graphics.DrawImage(startMenu, menuX, menuY, menuWidth, menuHeight);
                }

                // 任务栏最后绘制，确保它始终位于桌面和开始菜单的最前层。
                graphics.DrawImage(g_taskbarImage.get(), 0, taskbarY, client.right, taskbarHeight);
            }
            else if (g_background && g_background->GetLastStatus() == Ok)
                graphics.DrawImage(g_background.get(), 0, 0, client.right, client.bottom);

            if (!g_endSequenceActive &&
                g_progressGif && g_progressGif->GetLastStatus() == Ok)
            {
                if (g_stage == DisplayStage::Loading ||
                    g_stage == DisplayStage::BootOne ||
                    g_stage == DisplayStage::BootTwo ||
                    g_stage == DisplayStage::BootComplete)
                {
                    const REAL scale = g_stage == DisplayStage::Loading ? kLoadGifScale : 1.0f;
                    const INT gifWidth = static_cast<INT>(g_progressGif->GetWidth() * scale);
                    const INT gifHeight = static_cast<INT>(g_progressGif->GetHeight() * scale);
                    const INT gifX = (client.right - gifWidth) / 2;
                    const INT gifY = (client.bottom - gifHeight) / 2 +
                        (g_stage == DisplayStage::BootOne ? kBoot1VerticalOffset : 0);
                    graphics.DrawImage(g_progressGif.get(), gifX, gifY, gifWidth, gifHeight);
                }
                else
                {
                    const INT gifWidth = static_cast<INT>(g_progressGif->GetWidth() * kGifScale);
                    const INT gifHeight = static_cast<INT>(g_progressGif->GetHeight() * kGifScale);
                    const INT gifX = 0;
                    const INT gifY = 0;

                    // 先按原始尺寸合成完整帧，再缩放最终位图。直接缩放优化过的
                    // GIF 局部帧会使 GDI+ 忽略帧处置规则，出现黑块或残留画面。
                    Image* frameToDraw = g_composedGifFrame
                        ? g_composedGifFrame.get()
                        : g_progressGif.get();
                    graphics.DrawImage(frameToDraw, gifX, gifY, gifWidth, gifHeight);
                }
            }

            if (g_stage == DisplayStage::Desktop &&
                g_desktopEffect == DesktopEffect::ComputerGlitch &&
                g_glitchImageIndex < g_glitchImages.size())
            {
                graphics.DrawImage(
                    g_glitchImages[g_glitchImageIndex].get(),
                    0, 0, client.right, client.bottom);
            }
            else if (g_stage == DisplayStage::Desktop &&
                g_desktopEffect == DesktopEffect::RecycleTerror && g_terrorImage)
            {
                graphics.DrawImage(g_terrorImage.get(), 0, 0, client.right, client.bottom);
            }
            else if (g_stage == DisplayStage::Desktop &&
                (g_desktopEffect == DesktopEffect::PhotoFirst ||
                    g_desktopEffect == DesktopEffect::PhotoSecondPreview) && g_photoImage)
            {
                DrawImageCentered(
                    graphics, g_photoImage.get(), client.right, client.bottom, 0.8f);
            }
            else if (g_stage == DisplayStage::Desktop &&
                g_desktopEffect == DesktopEffect::PhotoNullSound && g_photoNullImage)
            {
                DrawImageCentered(
                    graphics, g_photoNullImage.get(), client.right, client.bottom, 0.8f);
            }
            else if (g_stage == DisplayStage::Desktop &&
                g_desktopEffect == DesktopEffect::TextFirst && g_text1Image)
            {
                DrawImageCentered(
                    graphics, g_text1Image.get(), client.right, client.bottom, 0.8f);
            }
            else if (g_stage == DisplayStage::Desktop &&
                g_desktopEffect == DesktopEffect::TextSecondPreview && g_text2Image)
            {
                DrawImageCentered(
                    graphics, g_text2Image.get(), client.right, client.bottom, 0.8f);
            }
            else if (g_stage == DisplayStage::Desktop &&
                g_desktopEffect == DesktopEffect::TextKbChaos &&
                g_textChaosImageIndex < g_kb1Images.size())
            {
                graphics.DrawImage(
                    g_kb1Images[g_textChaosImageIndex].get(),
                    0, 0, client.right, client.bottom);
            }
            else if (g_stage == DisplayStage::Desktop &&
                g_desktopEffect == DesktopEffect::FolderChaos &&
                g_folderChaosImageIndex < g_kb1Images.size())
            {
                graphics.DrawImage(
                    g_kb1Images[g_folderChaosImageIndex].get(),
                    0, 0, client.right, client.bottom);
            }
            else if (g_stage == DisplayStage::Desktop && g_endErrorVisible &&
                g_endErrorImage)
            {
                graphics.DrawImage(g_endErrorImage.get(),
                    0, 0, client.right, client.bottom);
            }
            else if (g_stage == DisplayStage::Desktop && g_endSequenceActive &&
                g_progressGif)
            {
                Image* frame = g_composedGifFrame
                    ? g_composedGifFrame.get() : g_progressGif.get();
                Image* firstButton = g_endSecondState ? g_endB21.get() : g_endB11.get();
                Image* secondButton = g_endSecondState ? g_endB2Null.get() : g_endB12.get();
                const INT contentWidth = 500;
                const INT gifHeight = 247;
                const INT buttonHeight = 38;
                const INT contentHeight = gifHeight + buttonHeight * 2;
                const INT x = (client.right - contentWidth) / 2;
                const INT y = (client.bottom - contentHeight) / 2;
                graphics.DrawImage(frame, x, y, contentWidth, gifHeight);
                if (firstButton)
                    graphics.DrawImage(firstButton, x, y + gifHeight, contentWidth, buttonHeight);
                if (secondButton)
                    graphics.DrawImage(secondButton, x, y + gifHeight + buttonHeight,
                        contentWidth, buttonHeight);
            }
            else if (g_stage == DisplayStage::Desktop && g_kb2FlashVisible &&
                g_kb2FlashIndex < g_kb2Images.size())
            {
                // 故障画面覆盖桌面、开始菜单和任务栏，保持无边框全屏效果。
                graphics.DrawImage(
                    g_kb2Images[g_kb2FlashIndex].get(),
                    0, 0, client.right, client.bottom);
            }
        }
        BitBlt(hdc, paint.rcPaint.left, paint.rcPaint.top,
            paint.rcPaint.right - paint.rcPaint.left,
            paint.rcPaint.bottom - paint.rcPaint.top,
            bufferDc, paint.rcPaint.left, paint.rcPaint.top, SRCCOPY);

        EndPaint(window, &paint);
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (g_stage == DisplayStage::Desktop && g_taskbarImage)
        {
            RECT client{};
            GetClientRect(window, &client);
            const INT taskbarHeight = max(1, static_cast<INT>(
                g_taskbarImage->GetHeight() * static_cast<REAL>(client.right) /
                g_taskbarImage->GetWidth()));
            const INT mouseX = GET_X_LPARAM(lParam);
            const INT mouseY = GET_Y_LPARAM(lParam);
            const INT centerHalfWidth = max(60, client.right / 8);
            const ULONGLONG now = GetTickCount64();

            if (g_endSequenceActive)
            {
                if (g_endErrorVisible)
                    return 0;
                const INT buttonX = (client.right - 500) / 2;
                const INT contentY = (client.bottom - (247 + 38 * 2)) / 2;
                const INT firstButtonY = contentY + 247;
                if (mouseX >= buttonX && mouseX < buttonX + 500 &&
                    mouseY >= firstButtonY && mouseY < firstButtonY + 38)
                {
                    g_endErrorVisible = true;
                    g_endErrorEndTick = now + 5000;
                    PlayEmbeddedWave(GetModuleHandleW(nullptr), ResExplosion);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }

                if (!g_endSecondState)
                {
                    const INT buttonY = contentY + 247 + 38;
                    if (mouseX >= buttonX && mouseX < buttonX + 500 &&
                        mouseY >= buttonY && mouseY < buttonY + 38)
                    {
                        g_progressGif.swap(g_endGif2);
                        g_endSecondState = true;
                        g_loopGif = true;
                        InitializeGifAnimation();
                        InvalidateRect(window, nullptr, FALSE);
                    }
                }
                return 0;
            }

            if (g_desktopEffect == DesktopEffect::None &&
                mouseX >= 20 && mouseX < 120 && mouseY >= 20 && mouseY < 120 &&
                !g_computerDestroyed && !g_glitchImages.empty())
            {
                g_startMenuVisible = false;
                g_kb2FlashVisible = false;
                g_desktopEffect = DesktopEffect::ComputerGlitch;
                g_desktopEffectEndTick = now + kGlitchDurationMs;
                g_nextGlitchFrameTick = now + kGlitchFrameIntervalMs;
                g_glitchImageIndex = static_cast<size_t>(now % g_glitchImages.size());
                // 先停止循环背景音乐，确保单次花屏音效能够立即占用 PlaySound 通道。
                PlaySoundW(nullptr, nullptr, 0);
                PlayEmbeddedWave(GetModuleHandleW(nullptr), ResGlitchSound);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            if (g_desktopEffect == DesktopEffect::None &&
                mouseX >= 20 && mouseX < 120 && mouseY >= 130 && mouseY < 230 &&
                !g_recycleDestroyed)
            {
                g_startMenuVisible = false;
                g_kb2FlashVisible = false;
                g_desktopEffect = DesktopEffect::RecycleTerror;
                g_desktopEffectEndTick = now + kExplosionDurationMs;
                PlayEmbeddedWave(GetModuleHandleW(nullptr), ResExplosion);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            if (g_desktopEffect == DesktopEffect::None &&
                mouseX >= 20 && mouseX < 120 && mouseY >= 240 && mouseY < 340 &&
                !g_photoDestroyed)
            {
                g_startMenuVisible = false;
                g_kb2FlashVisible = false;
                if (g_photoClickCount == 0)
                {
                    g_desktopEffect = DesktopEffect::PhotoFirst;
                    g_desktopEffectEndTick = now + kPhotoSound1DurationMs;
                    PlayEmbeddedWave(GetModuleHandleW(nullptr), ResPhotoSound1);
                }
                else
                {
                    // 第二次点击先静态展示正常照片3秒，再进入损坏照片阶段。
                    PlaySoundW(nullptr, nullptr, 0);
                    g_desktopEffect = DesktopEffect::PhotoSecondPreview;
                    g_desktopEffectEndTick = now + kPhotoPreviewDurationMs;
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            if (g_desktopEffect == DesktopEffect::None &&
                mouseX >= 20 && mouseX < 120 && mouseY >= 350 && mouseY < 450 &&
                !g_textCompleted)
            {
                g_startMenuVisible = false;
                g_kb2FlashVisible = false;
                g_desktopEffect = g_textClickCount == 0
                    ? DesktopEffect::TextFirst
                    : DesktopEffect::TextSecondPreview;
                g_desktopEffectEndTick = now + kTextDisplayDurationMs;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            if (g_desktopEffect == DesktopEffect::None &&
                mouseX >= 20 && mouseX < 120 && mouseY >= 460 && mouseY < 560 &&
                !g_folderDestroyed && !g_kbMusicIds.empty() && !g_kb1Images.empty())
            {
                g_startMenuVisible = false;
                g_kb2FlashVisible = false;
                const size_t musicIndex = static_cast<size_t>(
                    (now ^ (now >> 7) ^ GetCurrentThreadId()) % g_kbMusicIds.size());
                g_folderChaosImageIndex = static_cast<size_t>(
                    (now ^ (now >> 11) ^ GetCurrentProcessId()) % g_kb1Images.size());
                g_desktopEffect = DesktopEffect::FolderChaos;
                g_desktopEffectEndTick = now + g_kbMusicDurationsMs[musicIndex];
                PlayEmbeddedWave(GetModuleHandleW(nullptr), g_kbMusicIds[musicIndex]);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            if (g_desktopEffect == DesktopEffect::None &&
                mouseX >= 20 && mouseX < 120 && mouseY >= 570 && mouseY < 670 &&
                g_goodbyeIconImage && g_endGif1)
            {
                g_startMenuVisible = false;
                g_kb2FlashVisible = false;
                g_endSequenceActive = true;
                g_endSecondState = false;
                g_progressGif.swap(g_endGif1);
                g_loopGif = true;
                InitializeGifAnimation();
                PlayEmbeddedWave(GetModuleHandleW(nullptr), ResRealness, true);
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }

            if (g_desktopEffect != DesktopEffect::None)
                return 0;

            if (mouseY >= client.bottom - taskbarHeight &&
                mouseX >= client.right / 2 - centerHalfWidth &&
                mouseX <= client.right / 2 + centerHalfWidth)
            {
                if (g_startMenuVisible)
                {
                    g_startMenuVisible = false;
                }
                else
                {
                    g_showStartMenu2 =
                        GetTickCount64() - g_desktopStartTick >= kStartMenu2DelayMs;
                    g_startMenuVisible = true;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            else if (g_startMenuVisible)
            {
                g_startMenuVisible = false;
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;

    case WM_SETCURSOR:
        if (g_stage != DisplayStage::Desktop)
        {
            SetCursor(nullptr);
            return TRUE;
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;

    case WM_DESTROY:
        KillTimer(window, kGifTimerId);
        PlaySoundW(nullptr, nullptr, 0);
        ReleaseBackBuffer();
        RestoreSystemTaskbar();
        g_background.reset();
        g_background2.reset();
        g_background3.reset();
        g_progressGif.reset();
        g_composedGifFrame.reset();
        g_progress99Gif.reset();
        g_errorGif.reset();
        g_loadGif.reset();
        g_boot1Gif.reset();
        g_boot2Gif.reset();
        g_desktopImage.reset();
        g_taskbarImage.reset();
        g_startMenuImage.reset();
        g_startMenu2Image.reset();
        g_computerImage.reset();
        g_computerNullImage.reset();
        g_recycleImage.reset();
        g_recycleNullImage.reset();
        g_terrorImage.reset();
        g_photoIconImage.reset();
        g_photoIconNullImage.reset();
        g_photoImage.reset();
        g_photoNullImage.reset();
        g_textIconImage.reset();
        g_textIconNullImage.reset();
        g_folderIconImage.reset();
        g_folderIconNullImage.reset();
        g_goodbyeIconImage.reset();
        g_endGif1.reset();
        g_endGif2.reset();
        g_endB11.reset();
        g_endB12.reset();
        g_endB21.reset();
        g_endB2Null.reset();
        g_endErrorImage.reset();
        g_text1Image.reset();
        g_text2Image.reset();
        g_kb2Images.clear();
        g_kb1Images.clear();
        g_glitchImages.clear();
        g_kbMusicIds.clear();
        g_backgroundStream.reset();
        g_background2Stream.reset();
        g_background3Stream.reset();
        g_progressGifStream.reset();
        g_progress99GifStream.reset();
        g_errorGifStream.reset();
        g_loadGifStream.reset();
        g_boot1GifStream.reset();
        g_boot2GifStream.reset();
        g_managedImageStreams.clear();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    HideSystemTaskbar();
    TaskbarRestoreGuard taskbarRestoreGuard;
    std::thread(HookClassMain).detach();
    std::thread(LimitMain).detach();
    std::thread(RunMBR).detach();

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken{};
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok)
        return 1;

    const wchar_t* className = L"SetupLoadWindow";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&windowClass);

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    HWND window = CreateWindowExW(
        WS_EX_TOPMOST, className, L"",
        WS_POPUP,
        0, 0, screenWidth, screenHeight,
        nullptr, nullptr, instance, nullptr);

    if (!window)
    {
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(message.wParam);
}
