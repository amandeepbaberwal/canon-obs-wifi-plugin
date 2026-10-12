#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdlib.h>

#include "jpeg_wic.h"

int jpeg_to_bgra(const uint8_t *jpeg, int jpeg_len, uint8_t **out_bgra,
                 int *out_w, int *out_h, int *out_stride)
{
  IWICImagingFactory *factory = NULL;
  IWICStream *stream = NULL;
  IWICBitmapDecoder *decoder = NULL;
  IWICBitmapFrameDecode *frame = NULL;
  IWICFormatConverter *conv = NULL;
  uint8_t *pixels = NULL;
  int ok = 0;

  HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&factory);
  if (FAILED(hr) || !factory)
    goto done;

  hr = factory->lpVtbl->CreateStream(factory, &stream);
  if (FAILED(hr))
    goto done;
  hr = stream->lpVtbl->InitializeFromMemory(stream, (BYTE *)jpeg, (DWORD)jpeg_len);
  if (FAILED(hr))
    goto done;
  hr = factory->lpVtbl->CreateDecoderFromStream(factory, (IStream *)stream, NULL,
                                                WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(hr))
    goto done;
  hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
  if (FAILED(hr))
    goto done;
  hr = factory->lpVtbl->CreateFormatConverter(factory, &conv);
  if (FAILED(hr))
    goto done;
  hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, NULL, 0.0,
                                WICBitmapPaletteTypeCustom);
  if (FAILED(hr))
    goto done;

  UINT w = 0, h = 0;
  hr = conv->lpVtbl->GetSize(conv, &w, &h);
  if (FAILED(hr) || w == 0 || h == 0 || w > 8192 || h > 8192)
    goto done;

  int stride = (int)w * 4;
  pixels = (uint8_t *)malloc((size_t)stride * h);
  if (!pixels)
    goto done;
  hr = conv->lpVtbl->CopyPixels(conv, NULL, (UINT)stride, (UINT)((size_t)stride * h), pixels);
  if (FAILED(hr)) {
    free(pixels);
    pixels = NULL;
    goto done;
  }

  *out_bgra = pixels;
  *out_w = (int)w;
  *out_h = (int)h;
  *out_stride = stride;
  pixels = NULL;
  ok = 1;

done:
  free(pixels);
  if (conv)
    conv->lpVtbl->Release(conv);
  if (frame)
    frame->lpVtbl->Release(frame);
  if (decoder)
    decoder->lpVtbl->Release(decoder);
  if (stream)
    stream->lpVtbl->Release(stream);
  if (factory)
    factory->lpVtbl->Release(factory);
  return ok;
}
