/* Copyright 2017 Streampunk Media Ltd.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/* -LICENSE-START-
 ** Copyright (c) 2010 Blackmagic Design
 **
 ** Permission is hereby granted, free of charge, to any person or organization
 ** obtaining a copy of the software and accompanying documentation covered by
 ** this license (the "Software") to use, reproduce, display, distribute,
 ** execute, and transmit the Software, and to prepare derivative works of the
 ** Software, and to permit third-parties to whom the Software is furnished to
 ** do so, all subject to the following:
 **
 ** The copyright notices in the Software and this entire statement, including
 ** the above license grant, this restriction and the following disclaimer,
 ** must be included in all copies of the Software, in whole or in part, and
 ** all derivative works of the Software, unless such copies or derivative
 ** works are solely in the form of machine-executable object code generated by
 ** a source language processor.
 **
 ** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 ** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 ** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 ** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 ** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 ** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 ** DEALINGS IN THE SOFTWARE.
 ** -LICENSE-END-
 */

#include "Playback.h"
#include <string.h>

namespace streampunk {

using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::Persistent;
using v8::String;
using v8::Value;
using v8::EscapableHandleScope;
using v8::HandleScope;
using v8::Exception;

Persistent<Function> Playback::constructor;

Playback::Playback(uint32_t deviceIndex, uint32_t displayMode,
    uint32_t pixelFormat) : m_totalFrameScheduled(0), deviceIndex_(deviceIndex),
    displayMode_(displayMode), pixelFormat_(pixelFormat), result_(0) {
  async = new uv_async_t;
  uv_async_init(uv_default_loop(), async, FrameCallback);
  async->data = this;
}

Playback::~Playback() {
  if (!playbackCB_.IsEmpty())
    playbackCB_.Reset();
}

void Playback::Init(Local<Object> exports) {
  Isolate* isolate = exports->GetIsolate();

  #ifdef WIN32
  HRESULT result;
  result = CoInitialize(NULL);
	if (FAILED(result))
	{
		fprintf(stderr, "Initialization of COM failed - result = %08x.\n", result);
	}
  #endif

  // Prepare constructor template
  Local<FunctionTemplate> tpl = FunctionTemplate::New(isolate, New);
  tpl->SetClassName(String::NewFromUtf8(isolate, "Playback"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  // Prototype
  NODE_SET_PROTOTYPE_METHOD(tpl, "init", BMInit);
  NODE_SET_PROTOTYPE_METHOD(tpl, "scheduleFrame", ScheduleFrame);
  NODE_SET_PROTOTYPE_METHOD(tpl, "doPlayback", DoPlayback);
  NODE_SET_PROTOTYPE_METHOD(tpl, "stop", StopPlayback);

  constructor.Reset(isolate, tpl->GetFunction());
  exports->Set(String::NewFromUtf8(isolate, "Playback"),
               tpl->GetFunction());
}

void Playback::New(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.IsConstructCall()) {
    // Invoked as constructor: `new Playback(...)`
    uint32_t deviceIndex = args[0]->IsUndefined() ? 0 : args[0]->Uint32Value();
    uint32_t displayMode = args[1]->IsUndefined() ? 0 : args[1]->Uint32Value();
    uint32_t pixelFormat = args[2]->IsUndefined() ? 0 : args[2]->Uint32Value();
    Playback* obj = new Playback(deviceIndex, displayMode, pixelFormat);
    obj->Wrap(args.This());
    args.GetReturnValue().Set(args.This());
  } else {
    // Invoked as plain function `Playback(...)`, turn into construct call.
    const int argc = 3;
    Local<Value> argv[argc] = { args[0], args[1], args[2] };
    Local<Function> cons = Local<Function>::New(isolate, constructor);
    args.GetReturnValue().Set(cons->NewInstance(isolate->GetCurrentContext(), argc, argv).ToLocalChecked());
  }
}

void Playback::BMInit(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  IDeckLinkIterator* deckLinkIterator;
  HRESULT	result;
  IDeckLinkAPIInformation *deckLinkAPIInformation;
  IDeckLink* deckLink;
  #ifdef WIN32
  CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&deckLinkIterator);
  #else
  deckLinkIterator = CreateDeckLinkIteratorInstance();
  #endif
  result = deckLinkIterator->QueryInterface(IID_IDeckLinkAPIInformation, (void**)&deckLinkAPIInformation);
  if (result != S_OK) {
    isolate->ThrowException(Exception::Error(
      String::NewFromUtf8(isolate, "Error connecting to DeckLinkAPI.")));
  }
  Playback* obj = ObjectWrap::Unwrap<Playback>(args.Holder());

  for ( uint32_t x = 0 ; x <= obj->deviceIndex_ ; x++ ) {
    if (deckLinkIterator->Next(&deckLink) != S_OK) {
      args.GetReturnValue().Set(Undefined(isolate));
      return;
    }
  }

  obj->m_deckLink = deckLink;

  IDeckLinkOutput *deckLinkOutput;
  if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void **)&deckLinkOutput) != S_OK)
  {
    isolate->ThrowException(Exception::Error(
      String::NewFromUtf8(isolate, "Could not obtain DeckLink Output interface.\n")));
  }
  obj->m_deckLinkOutput = deckLinkOutput;

  if (obj->setupDeckLinkOutput())
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "made it!"));
  else
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "sad :-("));
}

void Playback::DoPlayback(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  Local<Function> cb = Local<Function>::Cast(args[0]);
  Playback* obj = ObjectWrap::Unwrap<Playback>(args.Holder());
  obj->playbackCB_.Reset(isolate, cb);

  int result = obj->m_deckLinkOutput->StartScheduledPlayback(0, obj->m_timeScale, 1.0);
  // printf("Playback result code %i and timescale %I64d.\n", result, obj->m_timeScale);

  if (result == S_OK) {
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Playback started."));
  }
  else {
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Playback failed to start."));
  }
}

void Playback::StopPlayback(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();

  Playback* obj = ObjectWrap::Unwrap<Playback>(args.Holder());

  obj->cleanupDeckLinkOutput();

  obj->playbackCB_.Reset();

  args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Playback stopped."));
}

void Playback::ScheduleFrame(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope scope(isolate);

  Playback* obj = ObjectWrap::Unwrap<Playback>(args.Holder());
  Local<Object> bufObj = args[0]->ToObject();

  uint32_t rowBytePixelRatioN = 1, rowBytePixelRatioD = 1;
  switch (obj->pixelFormat_) { // TODO expand to other pixel formats
    case bmdFormat10BitYUV:
      rowBytePixelRatioN = 8; rowBytePixelRatioD = 3;
      break;
    default:
      rowBytePixelRatioN = 2; rowBytePixelRatioD = 1;
      break;
  }

  IDeckLinkMutableVideoFrame* frame;
  if (obj->m_deckLinkOutput->CreateVideoFrame(obj->m_width, obj->m_height,
      obj->m_width * rowBytePixelRatioN / rowBytePixelRatioD,
      (BMDPixelFormat) obj->pixelFormat_, bmdFrameFlagDefault, &frame) != S_OK) {
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Failed to create frame."));
    return;
  };
  char* bufData = node::Buffer::Data(bufObj);
  size_t bufLength = node::Buffer::Length(bufObj);
  char* frameData = NULL;
  if (frame->GetBytes((void**) &frameData) != S_OK) {
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Failed to get new frame bytes."));
    return;
  };
  memcpy(frameData, bufData, bufLength);

  // printf("Frame duration %I64d/%I64d.\n", obj->m_frameDuration, obj->m_timeScale);

  HRESULT sfr = obj->m_deckLinkOutput->ScheduleVideoFrame(frame,
      (obj->m_totalFrameScheduled * obj->m_frameDuration),
      obj->m_frameDuration, obj->m_timeScale);
  if (sfr != S_OK) {
    printf("Failed to schedule frame. Code is %i.\n", sfr);
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Failed to schedule frame."));
    return;
  };

  obj->m_totalFrameScheduled++;
  args.GetReturnValue().Set(Number::New(isolate, obj->m_totalFrameScheduled));
}

bool Playback::setupDeckLinkOutput() {
  // bool							result = false;
  IDeckLinkDisplayModeIterator*	displayModeIterator = NULL;
  IDeckLinkDisplayMode*			deckLinkDisplayMode = NULL;

  m_width = -1;

  // set callback
  m_deckLinkOutput->SetScheduledFrameCompletionCallback(this);

  // get frame scale and duration for the video mode
  if (m_deckLinkOutput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
    return false;

  while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK)
  {
    if (deckLinkDisplayMode->GetDisplayMode() == displayMode_)
    {
      m_width = deckLinkDisplayMode->GetWidth();
      m_height = deckLinkDisplayMode->GetHeight();
      deckLinkDisplayMode->GetFrameRate(&m_frameDuration, &m_timeScale);
      deckLinkDisplayMode->Release();

      break;
    }

    deckLinkDisplayMode->Release();
  }

  displayModeIterator->Release();

  if (m_width == -1)
    return false;

  if (m_deckLinkOutput->EnableVideoOutput((BMDDisplayMode) displayMode_, bmdVideoOutputFlagDefault) != S_OK)
    return false;

  return true;
}

HRESULT	Playback::ScheduledFrameCompleted (IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result)
{
  result_ = result;
  uv_async_send(async);
  completedFrame->Release(); // Assume you should do this
	return S_OK;
}

void Playback::cleanupDeckLinkOutput()
{
	m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
	m_deckLinkOutput->DisableVideoOutput();
	m_deckLinkOutput->SetScheduledFrameCompletionCallback(NULL);
}

void Playback::FrameCallback(uv_async_t *handle) {
  Isolate* isolate = v8::Isolate::GetCurrent();
  HandleScope scope(isolate);
  Playback *playback = static_cast<Playback*>(handle->data);
  if (!playback->playbackCB_.IsEmpty()) {
    Local<Function> cb = Local<Function>::New(isolate, playback->playbackCB_);

    Local<Value> argv[1] = { Number::New(isolate, playback->result_) };
    cb->Call(Null(isolate), 1, argv);
  } else {
    printf("Frame callback is empty. Assuming finished.\n");
  }
}

}
