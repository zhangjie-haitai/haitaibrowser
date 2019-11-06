// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/plugins/renderer/loadable_plugin_placeholder.h"

#include <memory>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/json/string_escape.h"
#include "base/strings/string_piece.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/values.h"
#include "content/public/child/v8_value_converter.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_thread.h"
#include "third_party/WebKit/public/web/WebDOMMessageEvent.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebElement.h"
#include "third_party/WebKit/public/web/WebInputEvent.h"
#include "third_party/WebKit/public/web/WebKit.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebPluginContainer.h"
#include "third_party/WebKit/public/web/WebScriptSource.h"
#include "third_party/WebKit/public/web/WebSerializedScriptValue.h"
#include "third_party/WebKit/public/web/WebView.h"
#include "url/gurl.h"
#include "url/origin.h"

using base::UserMetricsAction;
using content::PluginInstanceThrottler;
using content::RenderThread;

namespace plugins {

void LoadablePluginPlaceholder::BlockForPowerSaverPoster() {
  DCHECK(!is_blocked_for_power_saver_poster_);
  is_blocked_for_power_saver_poster_ = true;

  render_frame()->RegisterPeripheralPlugin(
      url::Origin(GURL(GetPluginParams().url)),
      base::Bind(&LoadablePluginPlaceholder::MarkPluginEssential,
                 weak_factory_.GetWeakPtr(),
                 PluginInstanceThrottler::UNTHROTTLE_METHOD_BY_WHITELIST));
}

void LoadablePluginPlaceholder::SetPremadePlugin(
    content::PluginInstanceThrottler* throttler) {
  DCHECK(throttler);
  DCHECK(!premade_throttler_);
  premade_throttler_ = throttler;
}

LoadablePluginPlaceholder::LoadablePluginPlaceholder(
    content::RenderFrame* render_frame,
    blink::WebLocalFrame* frame,
    const blink::WebPluginParams& params,
    const std::string& html_data)
    : PluginPlaceholderBase(render_frame, frame, params, html_data),
      is_delayed_placeholder_(false),
      is_blocked_for_background_tab_(false),
      is_blocked_for_prerendering_(false),
      is_blocked_for_power_saver_poster_(false),
      power_saver_enabled_(false),
      premade_throttler_(nullptr),
      allow_loading_(false),
      finished_loading_(false),
      heuristic_run_before_(premade_throttler_ != nullptr),
      weak_factory_(this) {}

LoadablePluginPlaceholder::~LoadablePluginPlaceholder() {
}

void LoadablePluginPlaceholder::SetDelegate(
    std::unique_ptr<Delegate> delegate) {
  delegate_ = std::move(delegate);
}

void LoadablePluginPlaceholder::MarkPluginEssential(
    PluginInstanceThrottler::PowerSaverUnthrottleMethod method) {
  if (!power_saver_enabled_)
    return;

  power_saver_enabled_ = false;

  if (premade_throttler_)
    premade_throttler_->MarkPluginEssential(method);
  else if (method != PluginInstanceThrottler::UNTHROTTLE_METHOD_DO_NOT_RECORD)
    PluginInstanceThrottler::RecordUnthrottleMethodMetric(method);

  if (is_blocked_for_power_saver_poster_) {
    is_blocked_for_power_saver_poster_ = false;
    if (!LoadingBlocked())
      LoadPlugin();
  }
}

void LoadablePluginPlaceholder::ReplacePlugin(blink::WebPlugin* new_plugin) {
  CHECK(plugin());
  if (!new_plugin)
    return;
  blink::WebPluginContainer* container = plugin()->container();
  // This can occur if the container has been destroyed.
  if (!container) {
    new_plugin->destroy();
    return;
  }

  container->setPlugin(new_plugin);
  // Save the element in case the plugin is removed from the page during
  // initialization.
  blink::WebElement element = container->element();
  bool plugin_needs_initialization =
      !premade_throttler_ || new_plugin != premade_throttler_->GetWebPlugin();
  if (plugin_needs_initialization && !new_plugin->initialize(container)) {
    if (new_plugin->container()) {
      // Since the we couldn't initialize the new plugin, but the container
      // still exists, restore the placeholder and destroy the new plugin.
      container->setPlugin(plugin());
      new_plugin->destroy();
    }
    return;
  }

  // The plugin has been removed from the page. Destroy the old plugin. We
  // will be destroyed as soon as V8 garbage collects us.
  if (!element.pluginContainer()) {
    plugin()->destroy();
    return;
  }

  // During initialization, the new plugin might have replaced itself in turn
  // with another plugin. Make sure not to use the passed in |new_plugin| after
  // this point.
  new_plugin = container->plugin();

  plugin()->RestoreTitleText();
  container->invalidate();
  container->reportGeometry();
  plugin()->ReplayReceivedData(new_plugin);
  plugin()->destroy();
}

void LoadablePluginPlaceholder::SetMessage(const base::string16& message) {
  message_ = message;
  if (finished_loading_)
    UpdateMessage();
}

void LoadablePluginPlaceholder::UpdateMessage() {
  if (!plugin())
    return;
  std::string script =
      "window.setMessage(" + base::GetQuotedJSONString(message_) + ")";
  plugin()->web_view()->mainFrame()->executeScript(
      blink::WebScriptSource(base::UTF8ToUTF16(script)));
}

void LoadablePluginPlaceholder::PluginDestroyed() {
  if (power_saver_enabled_) {
    if (premade_throttler_) {
      // Since the premade plugin has been detached from the container, it will
      // not be automatically destroyed along with the page.
      premade_throttler_->GetWebPlugin()->destroy();
      premade_throttler_ = nullptr;
    } else if (is_blocked_for_power_saver_poster_) {
      // Record the NEVER unthrottle count only if there is no throttler.
      PluginInstanceThrottler::RecordUnthrottleMethodMetric(
          PluginInstanceThrottler::UNTHROTTLE_METHOD_NEVER);
    }

    // Prevent processing subsequent calls to MarkPluginEssential.
    power_saver_enabled_ = false;
  }

  PluginPlaceholderBase::PluginDestroyed();
}

v8::Local<v8::Object> LoadablePluginPlaceholder::GetV8ScriptableObject(
    v8::Isolate* isolate) const {
  // Pass through JavaScript access to the underlying throttled plugin.
  if (premade_throttler_ && premade_throttler_->GetWebPlugin()) {
    return premade_throttler_->GetWebPlugin()->v8ScriptableObject(isolate);
  }
  return v8::Local<v8::Object>();
}

void LoadablePluginPlaceholder::OnUnobscuredRectUpdate(
    const gfx::Rect& unobscured_rect) {
  DCHECK(content::RenderThread::Get());

  // TODO(groby): Handle the case of power saver not being enabled.
  if (!plugin() || !finished_loading_)
    return;

  if (unobscured_rect_ == unobscured_rect)
    return;

  unobscured_rect_ = unobscured_rect;

  float zoom_factor = plugin()->container()->pageZoomFactor();
  int width = roundf(unobscured_rect_.width() / zoom_factor);
  int height = roundf(unobscured_rect_.height() / zoom_factor);
  int x = roundf(unobscured_rect_.x() / zoom_factor);
  int y = roundf(unobscured_rect_.y() / zoom_factor);

  // On a size update check if we now qualify as a essential plugin.
  url::Origin content_origin = url::Origin(GetPluginParams().url);
  content::RenderFrame::PeripheralContentStatus status =
      render_frame()->GetPeripheralContentStatus(
          render_frame()->GetWebFrame()->top()->getSecurityOrigin(),
          content_origin, gfx::Size(width, height));
  // If this is a "delay" placeholder, delegate decisions.
  if (is_delayed_placeholder_) {
    OnLoadedRectUpdate(gfx::Rect(x, y, width, height), status);
    is_delayed_placeholder_ = false;
    return;
  }

  if (is_blocked_for_power_saver_poster_) {
    // Adjust poster container padding and dimensions to center play button for
    // plugins and plugin posters that have their top or left portions obscured.
    std::string script = base::StringPrintf(
        "window.resizePoster('%dpx', '%dpx', '%dpx', '%dpx')", x, y, width,
        height);
    plugin()->web_view()->mainFrame()->executeScript(
        blink::WebScriptSource(base::UTF8ToUTF16(script)));

    if (status != content::RenderFrame::CONTENT_STATUS_PERIPHERAL) {
      MarkPluginEssential(
          heuristic_run_before_
              ? PluginInstanceThrottler::UNTHROTTLE_METHOD_BY_SIZE_CHANGE
              : PluginInstanceThrottler::UNTHROTTLE_METHOD_DO_NOT_RECORD);

      if (!heuristic_run_before_ &&
          status ==
              content::RenderFrame::CONTENT_STATUS_ESSENTIAL_CROSS_ORIGIN_BIG) {
        render_frame()->WhitelistContentOrigin(content_origin);
      }
    }

    heuristic_run_before_ = true;
  }
}

void LoadablePluginPlaceholder::WasShown() {
  if (is_blocked_for_background_tab_) {
    is_blocked_for_background_tab_ = false;
    if (!LoadingBlocked())
      LoadPlugin();
  }
}

void LoadablePluginPlaceholder::OnLoadBlockedPlugins(
    const std::string& identifier) {
  if (!identifier.empty() && identifier != identifier_)
    return;

  RenderThread::Get()->RecordAction(UserMetricsAction("Plugin_Load_UI"));
  LoadPlugin();
}

void LoadablePluginPlaceholder::OnSetIsPrerendering(bool is_prerendering) {
  // Prerendering can only be enabled prior to a RenderView's first navigation,
  // so no BlockedPlugin should see the notification that enables prerendering.
  DCHECK(!is_prerendering);
  if (is_blocked_for_prerendering_) {
    is_blocked_for_prerendering_ = false;
    if (!LoadingBlocked())
      LoadPlugin();
  }
}

void LoadablePluginPlaceholder::LoadPlugin() {
  // This is not strictly necessary but is an important defense in case the
  // event propagation changes between "close" vs. "click-to-play".
  if (hidden())
    return;
  if (!plugin())
    return;
  if (!allow_loading_) {
    NOTREACHED();
    return;
  }

  if (premade_throttler_) {
    premade_throttler_->SetHiddenForPlaceholder(false /* hidden */);
    ReplacePlugin(premade_throttler_->GetWebPlugin());
    premade_throttler_ = nullptr;
  } else {
    ReplacePlugin(CreatePlugin());
  }
}

void LoadablePluginPlaceholder::LoadCallback() {
  RenderThread::Get()->RecordAction(UserMetricsAction("Plugin_Load_Click"));
  // If the user specifically clicks on the plugin content's placeholder,
  // disable power saver throttling for this instance.
  MarkPluginEssential(PluginInstanceThrottler::UNTHROTTLE_METHOD_BY_CLICK);
  LoadPlugin();
}

void LoadablePluginPlaceholder::DidFinishLoadingCallback() {
  finished_loading_ = true;
  if (message_.length() > 0)
    UpdateMessage();

  // Wait for the placeholder to finish loading to hide the premade plugin.
  // This is necessary to prevent a flicker.
  if (premade_throttler_ && power_saver_enabled_)
    premade_throttler_->SetHiddenForPlaceholder(true /* hidden */);

  // In case our initial geometry was reported before the placeholder finished
  // loading, request another one. Needed for correct large poster unthrottling.
  if (plugin()) {
    CHECK(plugin()->container());
    plugin()->container()->reportGeometry();
  }
}

void LoadablePluginPlaceholder::DidFinishIconRepositionForTestingCallback() {
  // Set an attribute and post an event, so browser tests can wait for the
  // placeholder to be ready to receive simulated user input.
  blink::WebElement element = plugin()->container()->element();
  element.setAttribute("placeholderReady", "true");

  std::unique_ptr<content::V8ValueConverter> converter(
      content::V8ValueConverter::create());
  base::StringValue value("placeholderReady");
  blink::WebSerializedScriptValue message_data =
      blink::WebSerializedScriptValue::serialize(converter->ToV8Value(
          &value, element.document().frame()->mainWorldScriptContext()));
  blink::WebDOMMessageEvent msg_event(message_data);

  plugin()->container()->enqueueMessageEvent(msg_event);
}

void LoadablePluginPlaceholder::SetPluginInfo(
    const content::WebPluginInfo& plugin_info) {
  plugin_info_ = plugin_info;
}

const content::WebPluginInfo& LoadablePluginPlaceholder::GetPluginInfo() const {
  return plugin_info_;
}

void LoadablePluginPlaceholder::SetIdentifier(const std::string& identifier) {
  identifier_ = identifier;
}

const std::string& LoadablePluginPlaceholder::GetIdentifier() const {
  return identifier_;
}

bool LoadablePluginPlaceholder::LoadingBlocked() const {
  DCHECK(allow_loading_);
  return is_blocked_for_background_tab_ || is_blocked_for_power_saver_poster_ ||
         is_blocked_for_prerendering_;
}

}  // namespace plugins