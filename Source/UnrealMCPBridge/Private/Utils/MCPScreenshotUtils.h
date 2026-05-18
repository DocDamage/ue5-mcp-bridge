// Copyright FatumGame. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FViewport;

/**
 * Phase 5 Chunk B — shared viewport-capture pipeline.
 *
 * Backs THREE wire tools:
 *   - ``editor.viewport_screenshot``           → in-memory base64 PNG/JPG (capped at 2048 either dim)
 *   - ``editor.viewport_screenshot_to_disk``   → file output (capped at 8192 either dim)
 *   - ``pie.screenshot_to_disk``               → PIE game-viewport file output
 *
 * Both viewport flavours funnel into the same ``CaptureViewport`` → ``EncodeImage`` pair so the
 * pixel-mungeing (alpha-opaque + optional resize + PNG/JPG encode) is single-sourced. The choice
 * of FViewport is the caller's concern (editor viewport vs. game viewport).
 *
 * **Threading: GAME THREAD ONLY.** ``Viewport->Draw()`` enqueues render-thread work and
 * ``Viewport->ReadPixels()`` flushes / waits for that work — both are GT-only by contract.
 *
 * **No FHighResScreenshotConfig path.** That subsystem renders via an async ``FImageWriteTask`` to
 * disk, with no synchronous in-memory return — incompatible with our request/response model.
 * We use the synchronous ``FViewport::Draw → ReadPixels`` route (the same path the ThumbnailManager
 * uses, see ``UThumbnailManager::CaptureProjectThumbnail``).
 *
 * **Resize policy.** When the requested ``DesiredWidth/DesiredHeight`` differ from the viewport's
 * native size, we resize via ``FImageUtils::ImageResize`` post-read. This avoids the much more
 * invasive route of temporarily resizing the viewport itself (which would disturb the editor's
 * actual presentation + force a layout pass). The cost is that "resolution" really means
 * "post-resize output size" — the underlying capture is at native viewport resolution.
 */
namespace FMCPScreenshotUtils
{
	/** Output image format selector. */
	enum class EImageFormat : uint8
	{
		PNG, // lossless; quality field is ignored
		JPG, // lossy; quality in [0, 100], 85 typical default
	};

	/**
	 * Capture ``Viewport`` to an RGBA8 byte array. On success:
	 *   - ``OutPixels`` is sized ``OutWidth * OutHeight * 4`` bytes
	 *   - ``OutWidth`` / ``OutHeight`` reflect the FINAL (possibly resized) dimensions
	 *
	 * Resize behavior:
	 *   - DesiredWidth/DesiredHeight = 0 → return native viewport size, no resize
	 *   - DesiredWidth/DesiredHeight > 0 AND differ from native → ImageResize to those dims
	 *
	 * Alpha post-processing: editor viewports leave A-channel garbage. We force opaque (A=255)
	 * via ``FImageCore::SetAlphaOpaque`` before returning so PNG/JPG encoders don't surface noise.
	 *
	 * Returns false on:
	 *   - null viewport
	 *   - native size 0×0 (viewport not yet realised — rare, on first-tick capture)
	 *   - ReadPixels failed (RHI not ready)
	 *
	 * Sets ``OutError`` with a human-readable cause string on failure. Always check before
	 * dereferencing OutPixels — partial population is possible on resize failure (rare).
	 */
	UNREALMCPBRIDGE_API bool CaptureViewport(
		FViewport* Viewport,
		int32 DesiredWidth,
		int32 DesiredHeight,
		TArray<uint8>& OutPixels,
		int32& OutWidth,
		int32& OutHeight,
		FString& OutError);

	/**
	 * Encode RGBA8 ``Pixels`` to PNG or JPG bytes. PNG ignores ``JpegQuality``; JPG uses
	 * [0, 100] with 85 a reasonable default.
	 *
	 * Returns false on:
	 *   - Pixels.Num() != Width*Height*4 (size mismatch — caller bug)
	 *   - Encoder returned empty (very rare; typically RGBA8→encoder roundtrip failure)
	 *
	 * The output is a ``TArray64<uint8>`` because high-res screenshots (8192×8192 PNG)
	 * can exceed 2 GiB on pathological inputs — though we cap at INT32_MAX before the
	 * file-write tier in caller-land.
	 */
	UNREALMCPBRIDGE_API bool EncodeImage(
		const TArray<uint8>& Pixels,
		int32 Width,
		int32 Height,
		EImageFormat Format,
		int32 JpegQuality,
		TArray64<uint8>& OutEncoded,
		FString& OutError);

	/**
	 * One-shot save: encode + write to disk. Creates parent directory (recursive) if needed.
	 * Returns false on encode failure OR write failure (e.g. disk full, permission denied).
	 *
	 * ``OutBytes`` reflects the encoded size (= bytes written on success).
	 *
	 * REFUSES when encoded size > INT32_MAX (~2 GiB) — UE's FFileHelper::SaveArrayToFile
	 * takes ``TArray<uint8>`` which is 32-bit-indexed; rather than chunking we surface the
	 * limit. PNG of a 8192×8192 RGBA8 image is ≤ ~120 MiB so the cap is effectively unreachable.
	 */
	UNREALMCPBRIDGE_API bool EncodeAndSaveToDisk(
		const TArray<uint8>& Pixels,
		int32 Width,
		int32 Height,
		EImageFormat Format,
		int32 JpegQuality,
		const FString& AbsPath,
		int64& OutBytes,
		FString& OutError);

	/**
	 * Parse the wire ``format`` field — accepts ``"png"`` / ``"jpg"`` / ``"jpeg"`` (case insensitive).
	 * Default (empty input) → PNG. Returns false on unrecognised input with descriptive error.
	 */
	UNREALMCPBRIDGE_API bool ParseFormat(const FString& Raw, EImageFormat& OutFormat, FString& OutError);
}
