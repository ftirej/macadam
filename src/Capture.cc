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

#include "Capture.h"

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

Persistent<Function> Capture::constructor;

Capture::Capture(uint32_t deviceIndex, uint32_t displayMode,
    uint32_t pixelFormat) : deviceIndex_(deviceIndex),
    displayMode_(displayMode), pixelFormat_(pixelFormat), latestFrame_(NULL) {
  async = new uv_async_t;
  uv_async_init(uv_default_loop(), async, FrameCallback);
  async->data = this;
}

Capture::~Capture() {
  if (!captureCB_.IsEmpty())
    captureCB_.Reset();
}

void Capture::Init(Local<Object> exports) {
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
  tpl->SetClassName(String::NewFromUtf8(isolate, "Capture"));
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  // Prototype
  NODE_SET_PROTOTYPE_METHOD(tpl, "init", BMInit);
  NODE_SET_PROTOTYPE_METHOD(tpl, "doCapture", DoCapture);
  NODE_SET_PROTOTYPE_METHOD(tpl, "stop", StopCapture);

  constructor.Reset(isolate, tpl->GetFunction());
  exports->Set(String::NewFromUtf8(isolate, "Capture"),
               tpl->GetFunction());
}

void Capture::New(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  if (args.IsConstructCall()) {
    // Invoked as constructor: `new Capture(...)`
    uint32_t deviceIndex = args[0]->IsUndefined() ? 0 : args[0]->Uint32Value();
    uint32_t displayMode = args[1]->IsUndefined() ? 0 : args[1]->Uint32Value();
    uint32_t pixelFormat = args[2]->IsUndefined() ? 0 : args[2]->Uint32Value();
    Capture* obj = new Capture(deviceIndex, displayMode, pixelFormat);
    obj->Wrap(args.This());
    args.GetReturnValue().Set(args.This());
  } else {
    // Invoked as plain function `Capture(...)`, turn into construct call.
    const int argc = 3;
    Local<Value> argv[argc] = { args[0], args[1], args[2] };
    Local<Function> cons = Local<Function>::New(isolate, constructor);
    args.GetReturnValue().Set(cons->NewInstance(isolate->GetCurrentContext(), argc, argv).ToLocalChecked());
  }
}

void Capture::BMInit(const FunctionCallbackInfo<Value>& args) {
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
  Capture* obj = ObjectWrap::Unwrap<Capture>(args.Holder());

  for ( uint32_t x = 0 ; x <= obj->deviceIndex_ ; x++ ) {
    if (deckLinkIterator->Next(&deckLink) != S_OK) {
      args.GetReturnValue().Set(Undefined(isolate));
      return;
    }
  }

  obj->m_deckLink = deckLink;

  IDeckLinkInput *deckLinkInput;
  if (deckLink->QueryInterface(IID_IDeckLinkInput, (void **)&deckLinkInput) != S_OK)
	{
    isolate->ThrowException(Exception::Error(
      String::NewFromUtf8(isolate, "Could not obtain the DeckLink Input interface.")));
	}
  obj->m_deckLinkInput = deckLinkInput;
  if (deckLinkInput)
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "made it!"));
  else
    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "sad :-("));
}

void Capture::DoCapture(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  Local<Function> cb = Local<Function>::Cast(args[0]);
  Capture* obj = ObjectWrap::Unwrap<Capture>(args.Holder());
  obj->captureCB_.Reset(isolate, cb);

  obj->setupDeckLinkInput();

  args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Capture started."));
}

void Capture::StopCapture(const v8::FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();

  Capture* obj = ObjectWrap::Unwrap<Capture>(args.Holder());

  obj->cleanupDeckLinkInput();

  args.GetReturnValue().Set(String::NewFromUtf8(isolate, "Capture stopped."));
}

// Stop video input
void Capture::cleanupDeckLinkInput()
{
	m_deckLinkInput->StopStreams();
	m_deckLinkInput->DisableVideoInput();
	m_deckLinkInput->SetCallback(NULL);
}

bool Capture::setupDeckLinkInput() {
  // bool result = false;
  IDeckLinkDisplayModeIterator*	displayModeIterator = NULL;
  IDeckLinkDisplayMode*			deckLinkDisplayMode = NULL;

  m_width = -1;

  // get frame scale and duration for the video mode
  if (m_deckLinkInput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
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

  printf("Width %li Height %li\n", m_width, m_height);

  displayModeIterator->Release();

  if (m_width == -1)
    return false;

  m_deckLinkInput->SetCallback(this);

  if (m_deckLinkInput->EnableVideoInput((BMDDisplayMode) displayMode_, (BMDPixelFormat) pixelFormat_, bmdVideoInputFlagDefault) != S_OK)
	  return false;

  if (m_deckLinkInput->StartStreams() != S_OK)
    return false;

  return true;
}

HRESULT	Capture::VideoInputFrameArrived (IDeckLinkVideoInputFrame* arrivedFrame, IDeckLinkAudioInputPacket*)
{
  arrivedFrame->AddRef();
  latestFrame_ = arrivedFrame;
  uv_async_send(async);
  return S_OK;
}

HRESULT	Capture::VideoInputFormatChanged (BMDVideoInputFormatChangedEvents notificationEvents, IDeckLinkDisplayMode* newDisplayMode, BMDDetectedVideoInputFormatFlags detectedSignalFlags) {
  return S_OK;
};

void Capture::TestUV() {
  uv_async_send(async);
}

// void FreeCallback(char* data, void* hint) {
//   Isolate* isolate = v8::Isolate::GetCurrent();
//   IDeckLinkVideoInputFrame* frame = static_cast<IDeckLinkVideoInputFrame*>(hint);
//   isolate->AdjustAmountOfExternalAllocatedMemory(-(frame->GetRowBytes() * frame->GetHeight()));
//   frame->Release();
// }

void Capture::FrameCallback(uv_async_t *handle) {
  Isolate* isolate = v8::Isolate::GetCurrent();
  HandleScope scope(isolate);
  Capture *capture = static_cast<Capture*>(handle->data);
  Local<Function> cb = Local<Function>::New(isolate, capture->captureCB_);
  char* new_data;
  capture->latestFrame_->GetBytes((void**) &new_data);
  long new_data_size = capture->latestFrame_->GetRowBytes() * capture->latestFrame_->GetHeight();
  // Local<Object> b = node::Buffer::New(isolate, new_data, new_data_size,
  //   FreeCallback, capture->latestFrame_).ToLocalChecked();
  Local<Object> b = node::Buffer::Copy(isolate, new_data, new_data_size).ToLocalChecked();
  capture->latestFrame_->Release();
  // long extSize = isolate->AdjustAmountOfExternalAllocatedMemory(new_data_size);
  // if (extSize > 100000000) {
  //   isolate->LowMemoryNotification();
  //   printf("Requesting bin collection.\n");
  // }
  Local<Value> argv[1] = { b };
  cb->Call(Null(isolate), 1, argv);
}

}
