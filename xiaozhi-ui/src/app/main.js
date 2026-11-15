/*
 * XiaoZhi agent UI for ESP-Brookesia (466x466 round AMOLED).
 *
 * Native bridge (system_core host_bridge):
 *   brookesia.start_service(name) -> handle            (only for bindable services)
 *   brookesia.call_function(handle, fn, paramsJson)
 *   brookesia.subscribe_event(handle, eventName)
 *   brookesia.call_service_function(service, fn, paramsJson)
 *   brookesia.subscribe_service_event(service, event)
 *
 * Two access paths are needed:
 *   - AgentManager is bindable=true: start_service() starts it and returns a handle,
 *     and every function call requires the service to be running.
 *   - XiaoZhi / Coze agents are bindable=false and owned by AgentManager, so they can
 *     only be reached by name (subscribe_service_event).
 *
 * The whole script is wrapped in an IIFE on purpose: the runtime may evaluate this
 * file again in the same JS context (app stopped and restarted), and top-level
 * const/let would then fail with "redeclaration of ...".
 */

// Service declarations for the toolchain (it scans this file for
// @brookesia/service/<name> references and writes them into manifest.services).
// Kept as comments on purpose: a real `import` would survive bundling and the
// device-side JS runtime would try to load the module path at run time.
// @brookesia/service/agent-manager
// @brookesia/service/agent-xiaozhi

(function () {
  'use strict';

  const AGENT_MANAGER = 'AgentManager';
  const XIAOZHI_SERVICE = 'XiaoZhi';
  const AGENT_NAME = 'XiaoZhi';

  // GUI paths must match the widget tree in res/screens/main.json:
  //   screen "main" -> container "panel" -> label "status"/"code"/"answer"/"hint"
  const GUI_STATUS = '/main/panel/status';
  const GUI_ANSWER = '/main/panel/answer';
  const GUI_USER = '/main/panel/user';
  const GUI_CODE = '/main/panel/code';
  const GUI_HINT = '/main/panel/hint';

  // Hardware gestures are forwarded by native code through System::dispatch_app_action(),
  // the same on_action() entry point a GUI control click uses. Apps may ignore it.
  const ACTION_APP_TOGGLE = 'app.toggle';
  const HINT_IDLE = '点击 Start 或双击 BOOT 键开始说话';

  let guiHandle = null;
  let managerHandle = null;
  let pendingStart = false;
  let agentState = 'Ready';

  function log() {
    const parts = Array.prototype.slice.call(arguments);
    parts.unshift('[xiaozhi]');
    try {
      brookesia.print.apply(null, parts);
    } catch (e) {
      /* ignore */
    }
  }

  function parseResult(raw, label) {
    if (!raw) {
      return null;
    }
    try {
      const parsed = JSON.parse(raw);
      if (parsed && parsed.success === false) {
        log(label, 'failed:', parsed.error);
      }
      return parsed;
    } catch (e) {
      log(label, 'bad result:', raw);
      return null;
    }
  }

  function callHandle(handle, fn, params) {
    if (handle === null) {
      log('skip', fn, '(no handle)');
      return null;
    }
    try {
      return parseResult(brookesia.call_function(handle, fn, JSON.stringify(params || {})), fn);
    } catch (e) {
      log(fn, 'exception:', e && e.message ? e.message : e);
      return null;
    }
  }

  function callService(service, fn, params) {
    try {
      return parseResult(
        brookesia.call_service_function(service, fn, JSON.stringify(params || {})),
        service + '.' + fn,
      );
    } catch (e) {
      log(service + '.' + fn, 'exception:', e && e.message ? e.message : e);
      return null;
    }
  }

  function subscribeHandleEvent(handle, event) {
    if (handle === null) {
      log('skip subscribe', event, '(no handle)');
      return null;
    }
    try {
      return brookesia.subscribe_event(handle, event);
    } catch (e) {
      log('subscribe', event, 'failed:', e && e.message ? e.message : e);
      return null;
    }
  }

  function subscribeService(service, event) {
    try {
      return brookesia.subscribe_service_event(service, event);
    } catch (e) {
      log('subscribe', service + '.' + event, 'failed:', e && e.message ? e.message : e);
      return null;
    }
  }

  function setText(path, text) {
    callHandle(guiHandle, 'SetText', { Path: path, Text: String(text) });
  }

  function setStatus(text) {
    log('status:', text);
    setText(GUI_STATUS, text);
  }

  // Cap the rendered text: the label is re-laid out on every update and long
  // strings cost both layout time and PSRAM for the glyph bitmap cache.
  function clampText(text, max_chars) {
    const value = String(text || '');
    return value.length > max_chars ? value.slice(0, max_chars) + '…' : value;
  }

  function setUser(text) {
    const value = clampText(text, 30);
    setText(GUI_USER, value ? '你：' + value : '');
  }

  function setReply(text) {
    const value = clampText(text, 120);
    setText(GUI_ANSWER, value ? '小智：' + value : '');
  }

  function callManager(fn, params) {
    return callHandle(managerHandle, fn, params);
  }

  function triggerGeneralAction(action) {
    const result = callManager('TriggerGeneralAction', { Action: action });
    log('action', action, '->', JSON.stringify(result));
    return result;
  }

  // General actions run asynchronously and only report that they were accepted.
  // Each action is gated on the previous general event, so activation is chained
  // through on_event() rather than fired back to back.
  function requestActivation() {
    const result = triggerGeneralAction('Activate');
    if (result && result.success === false) {
      // Agent requires an SNTP sync before activation (require_time_sync).
      setStatus('Syncing time...');
      triggerGeneralAction('TimeSync');
    }
  }

  function startSession() {
    // Replaying 'Activate' while the agent is already running is rejected by
    // the state machine ("No transition for action 'Activate' in state
    // 'Started'") but still reports success, which would leave the UI stuck.
    if (agentState === 'Started') {
      setStatus('Listening...');
      callManager('ManualStartListening', {});
      return;
    }
    if (agentState === 'Activating' || agentState === 'Starting') {
      setStatus('Please wait...');
      return;
    }
    pendingStart = true;
    agentState = 'Activating';
    setStatus('Activating...');
    requestActivation();
  }

  function stopSession() {
    if (agentState === 'Ready' || agentState === 'Stopped') {
      setStatus('Already stopped');
      return;
    }
    pendingStart = false;
    setStatus('Stopping...');
    triggerGeneralAction('Stop');
  }

  globalThis.brookesia_app = {
    on_start() {
      // Reset state: the script may be evaluated again after a previous run.
      guiHandle = null;
      managerHandle = null;
      pendingStart = false;
      agentState = 'Ready';

      guiHandle = brookesia.start_service('SystemGui');
      setStatus('Starting...');
      setUser('');
      setReply('');

      ['xz.start', 'xz.stop'].forEach(function (action) {
        callHandle(guiHandle, 'SubscribeAction', { Action: action });
      });

      // AgentManager is bindable: start_service() makes it running, which is a
      // precondition for every function call on it.
      try {
        managerHandle = brookesia.start_service(AGENT_MANAGER);
      } catch (e) {
        log('start AgentManager exception:', e && e.message ? e.message : e);
        managerHandle = null;
      }
      log('manager handle:', String(managerHandle));
      if (managerHandle === null) {
        setStatus('AgentManager unavailable');
        return true;
      }

      ['GeneralEventHappened', 'AgentSpeakingTextGot', 'UserSpeakingTextGot', 'ListeningStatusChanged'].forEach(
        function (event) {
          subscribeHandleEvent(managerHandle, event);
        },
      );
      // XiaoZhi is bindable=false and owned by AgentManager: subscribe by name.
      subscribeService(XIAOZHI_SERVICE, 'ActivationCodeReceived');

      const agents = callManager('GetAgentNames', {});
      log('agents:', JSON.stringify(agents));
      callManager('SetTargetAgent', { Name: AGENT_NAME });

      setStatus('Ready');
      return true;
    },

    on_action(action) {
      log('on_action', action);
      if (action === 'xz.start') {
        startSession();
      } else if (action === 'xz.stop') {
        stopSession();
      } else if (action === ACTION_APP_TOGGLE) {
        // Hardware gesture (BOOT double click): running -> stop, otherwise start.
        if (agentState === 'Started') {
          stopSession();
        } else {
          startSession();
        }
      } else if (action === 'xz.listen') {
        callManager('ManualStartListening', {});
      }
      return true;
    },

    on_event(service, event, dataJson) {
      let data = {};
      try {
        data = JSON.parse(dataJson || '{}');
      } catch (e) {
        data = {};
      }
      log('event', service, event, dataJson);

      if (event === 'ActivationCodeReceived') {
        setText(GUI_CODE, data.Code ? String(data.Code) : '');
        setStatus('Bind device on xiaozhi.me');
      } else if (event === 'GeneralEventHappened') {
        const name = String(data.Event || '');
        setStatus(name);
        if (name === 'TimeSynced') {
          agentState = 'Ready';
          setStatus('Activating...');
          triggerGeneralAction('Activate');
        } else if (name === 'Activated') {
          agentState = 'Activated';
          if (pendingStart) {
            pendingStart = false;
            // Clear the previous transcript only when a new round really starts,
            // so pressing Stop keeps the last conversation on screen.
            setUser('');
            setReply('');
            setStatus('Starting...');
            triggerGeneralAction('Start');
          }
        } else if (name === 'Started') {
          agentState = 'Started';
          pendingStart = false;
        } else if (name === 'Stopped') {
          agentState = 'Ready';
          pendingStart = false;
        }
      } else if (event === 'AgentSpeakingTextGot') {
        setReply(String(data.Text || ''));
      } else if (event === 'UserSpeakingTextGot') {
        setUser(String(data.Text || ''));
      } else if (event === 'ListeningStatusChanged') {
        setText(GUI_HINT, data.IsListening ? '正在聆听…' : HINT_IDLE);
      }
      return true;
    },

    on_pause() {
      return true;
    },

    on_resume() {
      return true;
    },

    on_stop() {
      triggerGeneralAction('Stop');
      return true;
    },
  };
})();
