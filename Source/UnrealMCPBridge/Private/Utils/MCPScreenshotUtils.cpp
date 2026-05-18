// Copyright FatumGame. All Rights Reserved.

#include "MCPScreenshotUtils.h"

#include "UnrealMCPBridge.h"

#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace
{
	/**
	 * Sanity bounds — Phase 5 plan caps in-memory variant at 2048, disk variant at 8192. We accept
	 * up to 16384 here as a defensive upper bound; tool-level argument parsers enforce the wire
	 * limits, this is the last-resort guard against caller bugs that bypass parsing.
	 */
	constexpr int32 kMaxResizeDim = 16384;

	/** Force all alpha bytes to 0xFF. Editor viewports leave A-channel garbage from translucent UI. */
	void ForceAlphaOpaque(TArray<uint8>& RGBA8, int32 Width, int32 Height)
	{
		const int32 PixelCount = Width * Height;
		check(RGBA8.Num() == PixelCount * 4);
		for (int32 i = 0; i < PixelCount; ++i)
		{
			// FColor in UE memory layout is B,G,R,A — but ImageCore::SetAlphaOpaque handles either
			// channel order. We hardwire 0xFF at offset 3 which works for both BGRA and RGBA since
			// alpha is always the last channel.
			RGBA8[i * 4 + 3] = 0xFF;
		}
	}
}

namespace FMCPScreenshotUtils
{

bool CaptureViewport(
	FViewport* Viewport,
	int32 DesiredWidth,
	int32 DesiredHeight,
	TArray<uint8>& OutPixels,
	int32& OutWidth,
	int32& OutHeight,
	FString& OutError)
{
	check(IsInGameThread());
	OutPixels.Reset();
	OutWidth = 0;
	OutHeight = 0;
	OutError.Reset();

	if (!Viewport)
	{
		OutError = TEXT("viewport is null");
		return false;
	}

	const FIntPoint NativeSize = Viewport->GetSizeXY();
	if (NativeSize.X <= 0 || NativeSize.Y <= 0)
	{
		OutError = FString::Printf(
			TEXT("viewport size 0×0 — not yet realised (size=%dx%d)"), NativeSize.X, NativeSize.Y);
		return false;
	}

	// Force a fresh draw before ReadPixels — without this we may sample stale backbuffer contents
	// (e.g. last frame's UI overlay). The Draw() call enqueues render commands; ReadPixels will
	// flush + wait. This matches the pattern in ViewportSelectionUtilities::PickColorAndAddLight.
	Viewport->Draw();

	TArray<FColor> NativeBitmap;
	if (!Viewport->ReadPixels(NativeBitmap))
	{
		OutError = TEXT("FViewport::ReadPixels failed (RHI not ready or readback unsupported)");
		return false;
	}
	const int32 ExpectedNativeBytes = NativeSize.X * NativeSize.Y;
	if (NativeBitmap.Num() != ExpectedNativeBytes)
	{
		OutError = FString::Printf(
			TEXT("ReadPixels returned %d FColor entries, expected %d (size %dx%d)"),
			NativeBitmap.Num(), ExpectedNativeBytes, NativeSize.X, NativeSize.Y);
		return false;
	}

	// Decide whether resize is needed. DesiredWidth/DesiredHeight = 0 means "use native".
	const bool bResize = (DesiredWidth > 0 && DesiredHeight > 0)
		&& (DesiredWidth != NativeSize.X || DesiredHeight != NativeSize.Y);
	if (bResize)
	{
		if (DesiredWidth > kMaxResizeDim || DesiredHeight > kMaxResizeDim)
		{
			OutError = FString::Printf(
				TEXT("requested size %dx%d exceeds internal max %d"),
				DesiredWidth, DesiredHeight, kMaxResizeDim);
			return false;
		}
		TArray<FColor> ResizedBitmap;
		// bResizeSRGBinLinearSpace=true → correct gamma for sRGB color content (the editor viewport
		// renders in sRGB). bForceOpaqueOutput=true → fills A=255 in the resized image so the
		// translucent UI artifacts in the source A channel don't leak through.
		FImageUtils::ImageResize(
			NativeSize.X, NativeSize.Y, NativeBitmap,
			DesiredWidth, DesiredHeight, ResizedBitmap,
			/*bResizeSRGBinLinearSpace*/ true, /*bForceOpaqueOutput*/ true);
		OutWidth = DesiredWidth;
		OutHeight = DesiredHeight;
		OutPixels.SetNumUninitialized(OutWidth * OutHeight * 4);
		FMemory::Memcpy(OutPixels.GetData(), ResizedBitmap.GetData(), OutPixels.Num());
	}
	else
	{
		OutWidth = NativeSize.X;
		OutHeight = NativeSize.Y;
		OutPixels.SetNumUninitialized(OutWidth * OutHeight * 4);
		FMemory::Memcpy(OutPixels.GetData(), NativeBitmap.GetData(), OutPixels.Num());
		// Resize path already forces opaque alpha; native path needs explicit sanitisation.
		ForceAlphaOpaque(OutPixels, OutWidth, OutHeight);
	}

	return true;
}

bool EncodeImage(
	const TArray<uint8>& Pixels,
	int32 Width,
	int32 Height,
	EImageFormat Format,
	int32 JpegQuality,
	TArray64<uint8>& OutEncoded,
	FString& OutError)
{
	OutEncoded.Reset();
	OutError.Reset();

	const int64 Expected = static_cast<int64>(Width) * static_cast<int64>(Height) * 4;
	if (Pixels.Num() != Expected)
	{
		OutError = FString::Printf(
			TEXT("pixel buffer size %d ≠ expected %lld (Width=%d Height=%d)"),
			Pixels.Num(), Expected, Width, Height);
		return false;
	}
	if (Width <= 0 || Height <= 0)
	{
		OutError = FString::Printf(TEXT("invalid dimensions %dx%d"), Width, Height);
		return false;
	}

	// Build an FImageView around the existing bytes (zero-copy). The view interprets pixels as
	// BGRA8 in sRGB color space, which matches the format ReadPixels emits.
	const FImageView View(
		reinterpret_cast<const FColor*>(Pixels.GetData()), Width, Height, EGammaSpace::sRGB);

	const TCHAR* FormatExt = (Format == EImageFormat::JPG) ? TEXT("jpg") : TEXT("png");
	// Clamp quality to [0, 100]. PNG ignores the value entirely. For JPG, 0 = max compression
	// (worst quality), 100 = least compression (best quality); we mirror the wire schema default
	// of 85 in the tool layer, callers may override.
	const int32 ClampedQuality = FMath::Clamp(JpegQuality, 0, 100);
	if (!FImageUtils::CompressImage(OutEncoded, FormatExt, View, ClampedQuality))
	{
		OutError = FString::Printf(
			TEXT("%s encode failed (FImageUtils::CompressImage returned false)"), FormatExt);
		return false;
	}
	if (OutEncoded.Num() == 0)
	{
		OutError = FString::Printf(
			TEXT("%s encode produced 0 bytes (encoder did not report failure)"), FormatExt);
		return false;
	}
	return true;
}

bool EncodeAndSaveToDisk(
	const TArray<uint8>& Pixels,
	int32 Width,
	int32 Height,
	EImageFormat Format,
	int32 JpegQuality,
	const FString& AbsPath,
	int64& OutBytes,
	FString& OutError)
{
	OutBytes = 0;
	OutError.Reset();

	TArray64<uint8> Encoded;
	if (!EncodeImage(Pixels, Width, Height, Format, JpegQuality, Encoded, OutError))
	{
		return false;
	}
	if (Encoded.Num() > INT32_MAX)
	{
		OutError = FString::Printf(
			TEXT("encoded image %lld bytes exceeds 2 GiB limit — refusing to write"),
			Encoded.Num());
		return false;
	}

	// Ensure parent directory exists; SaveArrayToFile silently fails otherwise.
	const FString ParentDir = FPaths::GetPath(AbsPath);
	if (!ParentDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*ParentDir, /*Tree*/ true);
	}

	TArray<uint8> WriteBuf;
	WriteBuf.Append(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
	if (!FFileHelper::SaveArrayToFile(WriteBuf, *AbsPath))
	{
		OutError = FString::Printf(TEXT("could not write '%s' to disk"), *AbsPath);
		return false;
	}
	OutBytes = WriteBuf.Num();
	return true;
}

bool ParseFormat(const FString& Raw, EImageFormat& OutFormat, FString& OutError)
{
	if (Raw.IsEmpty() || Raw.Equals(TEXT("png"), ESearchCase::IgnoreCase))
	{
		OutFormat = EImageFormat::PNG;
		return true;
	}
	if (Raw.Equals(TEXT("jpg"), ESearchCase::IgnoreCase)
		|| Raw.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase))
	{
		OutFormat = EImageFormat::JPG;
		return true;
	}
	OutError = FString::Printf(
		TEXT("format '%s' invalid — accepted: png | jpg (alias: jpeg)"), *Raw);
	return false;
}

} // namespace FMCPScreenshotUtils
