#include "http_server.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "tts.h"
#include "stt.h"
#include "live_stt.h"
#include "openai_live_stt.h"
#include "bsp_board_extra.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "HTTP_SERVER";
static httpd_handle_t server = NULL;
static bool tts_initialized = false;
static bool stt_initialized = false;

/* Common CSS for sidebar layout - must be a macro for string concatenation */
#define COMMON_CSS \
    "<style>" \
    "* { box-sizing: border-box; margin: 0; padding: 0; }" \
    "body { font-family: Arial, sans-serif; background: #f5f5f5; display: flex; min-height: 100vh; }" \
    ".sidebar { width: 220px; background: #2c3e50; color: white; position: fixed; height: 100vh; padding: 20px 0; }" \
    ".sidebar .logo { font-size: 18px; font-weight: bold; padding: 0 20px 20px; border-bottom: 1px solid #34495e; }" \
    ".sidebar nav { margin-top: 20px; }" \
    ".sidebar .nav-item { display: flex; align-items: center; padding: 12px 20px; color: #bdc3c7; text-decoration: none; transition: all 0.2s; }" \
    ".sidebar .nav-item:hover { background: #34495e; color: white; }" \
    ".sidebar .nav-item.active { background: #3498db; color: white; }" \
    ".sidebar .nav-item .icon { margin-right: 10px; font-size: 18px; }" \
    ".main-content { margin-left: 220px; flex: 1; padding: 30px; }" \
    ".page-header { margin-bottom: 25px; }" \
    ".page-header h1 { color: #2c3e50; font-size: 28px; margin-bottom: 5px; }" \
    ".page-header .subtitle { color: #7f8c8d; }" \
    ".card { background: white; border-radius: 10px; padding: 25px; box-shadow: 0 2px 10px rgba(0,0,0,0.05); margin-bottom: 20px; }" \
    ".card h2 { color: #2c3e50; font-size: 18px; margin-bottom: 15px; }" \
    ".status-bar { padding: 12px 15px; background: #e8f5e9; border-radius: 5px; margin-bottom: 20px; }" \
    ".status-bar.warning { background: #fff3e0; }" \
    ".status-bar.error { background: #ffebee; }" \
    "textarea { width: 100%; height: 100px; padding: 12px; font-size: 15px; border: 1px solid #ddd; border-radius: 5px; resize: vertical; }" \
    "select { width: 100%; padding: 10px; font-size: 15px; border: 1px solid #ddd; border-radius: 5px; }" \
    "button { background: #3498db; color: white; padding: 12px 24px; border: none; border-radius: 5px; cursor: pointer; font-size: 15px; transition: background 0.2s; }" \
    "button:hover { background: #2980b9; }" \
    "button:disabled { background: #bdc3c7; cursor: not-allowed; }" \
    "button.success { background: #27ae60; }" \
    "button.success:hover { background: #219a52; }" \
    "button.danger { background: #e74c3c; }" \
    "button.danger:hover { background: #c0392b; }" \
    "button.secondary { background: #95a5a6; }" \
    "button.secondary:hover { background: #7f8c8d; }" \
    ".control-group { margin-bottom: 15px; }" \
    ".control-group label { display: block; margin-bottom: 5px; font-weight: 600; color: #34495e; }" \
    ".slider-row { display: flex; align-items: center; gap: 10px; }" \
    ".slider-row input[type=range] { flex: 1; }" \
    ".slider-row span { min-width: 50px; text-align: right; color: #7f8c8d; }" \
    ".result { padding: 15px; border-radius: 5px; margin-top: 15px; }" \
    ".result.success { background: #e8f5e9; color: #2e7d32; }" \
    ".result.error { background: #ffebee; color: #c62828; }" \
    ".result.info { background: #e3f2fd; color: #1565c0; }" \
    ".result.warning { background: #fff3e0; color: #e65100; }" \
    ".timer { font-size: 48px; font-weight: bold; text-align: center; font-family: monospace; color: #2c3e50; margin: 20px 0; }" \
    ".transcript-box { min-height: 150px; max-height: 400px; overflow-y: auto; padding: 15px; background: #fafafa; border: 1px solid #eee; border-radius: 5px; white-space: pre-wrap; word-wrap: break-word; }" \
    ".settings-row { display: flex; justify-content: space-between; align-items: center; padding: 12px 0; border-bottom: 1px solid #eee; }" \
    ".settings-row:last-child { border-bottom: none; }" \
    ".settings-row .label { color: #34495e; font-weight: 500; }" \
    ".settings-row .value { color: #7f8c8d; }" \
    ".settings-row .value.configured { color: #27ae60; }" \
    ".settings-row .value.not-configured { color: #e74c3c; }" \
    "@media (max-width: 768px) { .sidebar { width: 60px; } .sidebar .logo, .sidebar .nav-text { display: none; } .sidebar .nav-item { justify-content: center; padding: 15px; } .sidebar .nav-item .icon { margin: 0; } .main-content { margin-left: 60px; } }" \
    "</style>"

/* Sidebar HTML component */
#define SIDEBAR_HTML(active_page) \
    "<div class=\"sidebar\">" \
    "<div class=\"logo\">ESP32-P4 Audio</div>" \
    "<nav>" \
    "<a href=\"/\" class=\"nav-item" active_page##_TTS "\"><span class=\"icon\">&#128266;</span><span class=\"nav-text\">Text-to-Speech</span></a>" \
    "<a href=\"/stt\" class=\"nav-item" active_page##_STT "\"><span class=\"icon\">&#128221;</span><span class=\"nav-text\">Batch STT</span></a>" \
    "<a href=\"/live\" class=\"nav-item" active_page##_LIVE "\"><span class=\"icon\">&#127908;</span><span class=\"nav-text\">Live STT (DG)</span></a>" \
    "<a href=\"/openai-live\" class=\"nav-item" active_page##_OPENAI "\"><span class=\"icon\">&#127897;</span><span class=\"nav-text\">Live STT (OpenAI)</span></a>" \
    "<a href=\"/settings\" class=\"nav-item" active_page##_SETTINGS "\"><span class=\"icon\">&#9881;</span><span class=\"nav-text\">Settings</span></a>" \
    "</nav>" \
    "</div>"

#define ACTIVE_TTS_TTS " active"
#define ACTIVE_TTS_STT ""
#define ACTIVE_TTS_LIVE ""
#define ACTIVE_TTS_OPENAI ""
#define ACTIVE_TTS_SETTINGS ""

#define ACTIVE_STT_TTS ""
#define ACTIVE_STT_STT " active"
#define ACTIVE_STT_LIVE ""
#define ACTIVE_STT_OPENAI ""
#define ACTIVE_STT_SETTINGS ""

#define ACTIVE_LIVE_TTS ""
#define ACTIVE_LIVE_STT ""
#define ACTIVE_LIVE_LIVE " active"
#define ACTIVE_LIVE_OPENAI ""
#define ACTIVE_LIVE_SETTINGS ""

#define ACTIVE_OPENAI_TTS ""
#define ACTIVE_OPENAI_STT ""
#define ACTIVE_OPENAI_LIVE ""
#define ACTIVE_OPENAI_OPENAI " active"
#define ACTIVE_OPENAI_SETTINGS ""

#define ACTIVE_SETTINGS_TTS ""
#define ACTIVE_SETTINGS_STT ""
#define ACTIVE_SETTINGS_LIVE ""
#define ACTIVE_SETTINGS_OPENAI ""
#define ACTIVE_SETTINGS_SETTINGS " active"

/* TTS Page HTML */
static const char *TTS_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Text-to-Speech - ESP32-P4</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    COMMON_CSS
    "</head>"
    "<body>"
    SIDEBAR_HTML(ACTIVE_TTS)
    "<div class=\"main-content\">"
    "<div class=\"page-header\">"
    "<h1>Text-to-Speech</h1>"
    "<p class=\"subtitle\">Convert text to spoken audio</p>"
    "</div>"
    "<div class=\"card\">"
    "<div class=\"status-bar\">"
    "<strong>Provider:</strong> <span id=\"currentProvider\">Loading...</span>"
    "</div>"
    "<form id=\"ttsForm\">"
    "<div class=\"control-group\">"
    "<label for=\"text\">Enter text to speak:</label>"
    "<textarea id=\"text\" name=\"text\" placeholder=\"Type something here...\"></textarea>"
    "</div>"
    "<div class=\"control-group\">"
    "<label for=\"provider\">TTS Provider:</label>"
    "<select id=\"provider\" name=\"provider\">"
    "<option value=\"0\">Loading...</option>"
    "</select>"
    "</div>"
    "<div class=\"control-group\">"
    "<label for=\"speed\">Speech Speed:</label>"
    "<div class=\"slider-row\">"
    "<input type=\"range\" id=\"speed\" name=\"speed\" min=\"0.5\" max=\"2.0\" step=\"0.1\" value=\"1.0\">"
    "<span id=\"speedVal\">1.0x</span>"
    "</div>"
    "</div>"
    "<div class=\"control-group\">"
    "<label for=\"volume\">Volume:</label>"
    "<div class=\"slider-row\">"
    "<input type=\"range\" id=\"volume\" name=\"volume\" min=\"0\" max=\"100\" step=\"5\" value=\"80\">"
    "<span id=\"volumeVal\">80%</span>"
    "</div>"
    "</div>"
    "<button type=\"submit\" id=\"speakBtn\" class=\"success\">Speak</button>"
    "</form>"
    "<div id=\"result\" class=\"result\" style=\"display:none\"></div>"
    "</div>"
    "</div>"
    "<script>"
    "const speedSlider = document.getElementById('speed');"
    "const speedVal = document.getElementById('speedVal');"
    "const volumeSlider = document.getElementById('volume');"
    "const volumeVal = document.getElementById('volumeVal');"
    "const providerSelect = document.getElementById('provider');"
    "const currentProviderSpan = document.getElementById('currentProvider');"
    "let currentProvider = 0;"
    "function updateSpeedRange() {"
    "  if (currentProvider === 1) {"
    "    speedSlider.min = '0.25';"
    "    speedSlider.max = '4.0';"
    "  } else {"
    "    speedSlider.min = '0.5';"
    "    speedSlider.max = '2.0';"
    "    if (parseFloat(speedSlider.value) > 2.0) speedSlider.value = '2.0';"
    "    if (parseFloat(speedSlider.value) < 0.5) speedSlider.value = '0.5';"
    "  }"
    "  speedVal.textContent = speedSlider.value + 'x';"
    "}"
    "async function loadProviders() {"
    "  try {"
    "    const response = await fetch('/api/provider');"
    "    const data = await response.json();"
    "    providerSelect.innerHTML = '';"
    "    data.providers.forEach(p => {"
    "      const opt = document.createElement('option');"
    "      opt.value = p.id;"
    "      opt.textContent = p.name + (p.available ? '' : ' (not configured)');"
    "      opt.disabled = !p.available;"
    "      if (p.id === data.current) opt.selected = true;"
    "      providerSelect.appendChild(opt);"
    "    });"
    "    currentProvider = data.current;"
    "    currentProviderSpan.textContent = data.providers.find(p => p.id === data.current)?.name || 'Unknown';"
    "    updateSpeedRange();"
    "  } catch (err) { console.error('Failed to load providers:', err); }"
    "}"
    "providerSelect.addEventListener('change', async function() {"
    "  try {"
    "    const response = await fetch('/api/provider', {"
    "      method: 'POST',"
    "      headers: { 'Content-Type': 'application/json' },"
    "      body: JSON.stringify({ provider: parseInt(this.value) })"
    "    });"
    "    const data = await response.json();"
    "    if (response.ok) {"
    "      currentProvider = data.provider;"
    "      currentProviderSpan.textContent = data.name;"
    "      updateSpeedRange();"
    "    } else {"
    "      alert('Failed to change provider: ' + data.error);"
    "      loadProviders();"
    "    }"
    "  } catch (err) { console.error('Provider change error:', err); }"
    "});"
    "speedSlider.addEventListener('input', function() {"
    "  speedVal.textContent = this.value + 'x';"
    "});"
    "volumeSlider.addEventListener('input', function() {"
    "  volumeVal.textContent = this.value + '%';"
    "});"
    "volumeSlider.addEventListener('change', async function() {"
    "  try {"
    "    await fetch('/api/volume', {"
    "      method: 'POST',"
    "      headers: { 'Content-Type': 'application/json' },"
    "      body: JSON.stringify({ volume: parseInt(this.value) })"
    "    });"
    "  } catch (err) { console.error('Volume error:', err); }"
    "});"
    "document.getElementById('ttsForm').addEventListener('submit', async function(e) {"
    "  e.preventDefault();"
    "  const text = document.getElementById('text').value.trim();"
    "  if (!text) { alert('Please enter some text'); return; }"
    "  const speed = parseFloat(document.getElementById('speed').value);"
    "  const btn = document.getElementById('speakBtn');"
    "  const result = document.getElementById('result');"
    "  btn.disabled = true;"
    "  btn.textContent = 'Speaking...';"
    "  result.className = 'result warning';"
    "  result.style.display = 'block';"
    "  result.textContent = 'Generating speech...';"
    "  try {"
    "    const response = await fetch('/api/tts', {"
    "      method: 'POST',"
    "      headers: { 'Content-Type': 'application/json' },"
    "      body: JSON.stringify({ text: text, speed: speed })"
    "    });"
    "    const data = await response.json();"
    "    if (response.ok) {"
    "      result.className = 'result success';"
    "      result.textContent = 'Speech completed!';"
    "    } else {"
    "      result.className = 'result error';"
    "      result.textContent = 'Error: ' + (data.error || 'Unknown error');"
    "    }"
    "  } catch (err) {"
    "    result.className = 'result error';"
    "    result.textContent = 'Network error: ' + err.message;"
    "  }"
    "  btn.disabled = false;"
    "  btn.textContent = 'Speak';"
    "});"
    "loadProviders();"
    "</script>"
    "</body>"
    "</html>";

/* STT (Batch) HTML page content */
static const char *STT_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Batch STT - ESP32-P4</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    COMMON_CSS
    "<style>"
    ".record-btn { width: 100%; }"
    ".record-btn.recording { background: #27ae60; animation: pulse 1s infinite; }"
    "@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }"
    "</style>"
    "</head>"
    "<body>"
    SIDEBAR_HTML(ACTIVE_STT)
    "<div class=\"main-content\">"
    "<div class=\"page-header\">"
    "<h1>Batch Speech-to-Text</h1>"
    "<p class=\"subtitle\">Record audio, then transcribe via OpenAI Whisper</p>"
    "</div>"
    "<div class=\"card\">"
    "<div class=\"status-bar\">"
    "<strong>Status:</strong> <span id=\"stateText\">Idle</span> | <strong>Max Recording:</strong> 5 minutes"
    "</div>"
    "<div class=\"timer\" id=\"timer\">00:00</div>"
    "<button id=\"recordBtn\" class=\"record-btn danger\">Start Recording</button>"
    "<button id=\"resetBtn\" class=\"secondary\" style=\"display:none;width:100%;margin-top:10px\">New Recording</button>"
    "<div id=\"result\" class=\"transcript-box\" style=\"margin-top:20px\">Press the button to start recording. Speak clearly into the microphone.</div>"
    "</div>"
    "</div>"
    "<script>"
    "let isRecording = false;"
    "let timerInterval = null;"
    "let startTime = 0;"
    "let pollInterval = null;"
    "const recordBtn = document.getElementById('recordBtn');"
    "const resetBtn = document.getElementById('resetBtn');"
    "const result = document.getElementById('result');"
    "const stateText = document.getElementById('stateText');"
    "const timer = document.getElementById('timer');"
    "function updateTimer() {"
    "  const elapsed = Math.floor((Date.now() - startTime) / 1000);"
    "  const mins = Math.floor(elapsed / 60).toString().padStart(2, '0');"
    "  const secs = (elapsed % 60).toString().padStart(2, '0');"
    "  timer.textContent = mins + ':' + secs;"
    "}"
    "async function startRecording() {"
    "  try {"
    "    const resp = await fetch('/api/stt/start', { method: 'POST' });"
    "    const data = await resp.json();"
    "    if (resp.ok) {"
    "      isRecording = true;"
    "      recordBtn.textContent = 'Stop Recording';"
    "      recordBtn.classList.add('recording');"
    "      result.style.background = '#fff3e0';"
    "      result.textContent = 'Recording... Speak now!';"
    "      stateText.textContent = 'Recording';"
    "      resetBtn.style.display = 'none';"
    "      startTime = Date.now();"
    "      timerInterval = setInterval(updateTimer, 100);"
    "    } else {"
    "      result.style.background = '#ffebee';"
    "      result.textContent = 'Error: ' + (data.error || 'Failed to start recording');"
    "    }"
    "  } catch (err) {"
    "    result.style.background = '#ffebee';"
    "    result.textContent = 'Network error: ' + err.message;"
    "  }"
    "}"
    "async function stopRecording() {"
    "  clearInterval(timerInterval);"
    "  isRecording = false;"
    "  recordBtn.disabled = true;"
    "  recordBtn.textContent = 'Processing...';"
    "  recordBtn.classList.remove('recording');"
    "  result.style.background = '#e3f2fd';"
    "  result.textContent = 'Uploading and transcribing audio...';"
    "  stateText.textContent = 'Transcribing';"
    "  try {"
    "    await fetch('/api/stt/stop', { method: 'POST' });"
    "    pollInterval = setInterval(pollStatus, 500);"
    "  } catch (err) {"
    "    recordBtn.disabled = false;"
    "    recordBtn.textContent = 'Start Recording';"
    "    result.style.background = '#ffebee';"
    "    result.textContent = 'Network error: ' + err.message;"
    "  }"
    "}"
    "async function pollStatus() {"
    "  try {"
    "    const resp = await fetch('/api/stt/status');"
    "    const data = await resp.json();"
    "    if (data.state === 'done') {"
    "      clearInterval(pollInterval);"
    "      recordBtn.style.display = 'none';"
    "      resetBtn.style.display = 'block';"
    "      result.style.background = '#e8f5e9';"
    "      result.textContent = data.transcription || '(No speech detected)';"
    "      stateText.textContent = 'Done';"
    "    } else if (data.state === 'error') {"
    "      clearInterval(pollInterval);"
    "      recordBtn.disabled = false;"
    "      recordBtn.textContent = 'Start Recording';"
    "      result.style.background = '#ffebee';"
    "      result.textContent = 'Error: ' + (data.error || 'Unknown error');"
    "      stateText.textContent = 'Error';"
    "    } else if (data.state === 'transcribing') {"
    "      result.textContent = 'Uploading and transcribing audio... (' + Math.round(data.audio_bytes/1024) + ' KB)';"
    "    }"
    "  } catch (err) { console.error('Poll error:', err); }"
    "}"
    "async function resetSTT() {"
    "  try {"
    "    await fetch('/api/stt/reset', { method: 'POST' });"
    "  } catch (err) { console.error('Reset error:', err); }"
    "  recordBtn.style.display = 'block';"
    "  recordBtn.disabled = false;"
    "  recordBtn.textContent = 'Start Recording';"
    "  resetBtn.style.display = 'none';"
    "  result.style.background = '#fafafa';"
    "  result.textContent = 'Press the button to start recording. Speak clearly into the microphone.';"
    "  stateText.textContent = 'Idle';"
    "  timer.textContent = '00:00';"
    "}"
    "recordBtn.addEventListener('click', function() {"
    "  if (isRecording) { stopRecording(); }"
    "  else { startRecording(); }"
    "});"
    "resetBtn.addEventListener('click', resetSTT);"
    "async function checkInitialState() {"
    "  try {"
    "    const resp = await fetch('/api/stt/status');"
    "    const data = await resp.json();"
    "    if (data.state === 'recording') {"
    "      isRecording = true;"
    "      recordBtn.textContent = 'Stop Recording';"
    "      recordBtn.classList.add('recording');"
    "      result.style.background = '#fff3e0';"
    "      result.textContent = 'Recording in progress...';"
    "      stateText.textContent = 'Recording';"
    "      startTime = Date.now() - data.recording_ms;"
    "      timerInterval = setInterval(updateTimer, 100);"
    "    } else if (data.state === 'transcribing') {"
    "      recordBtn.disabled = true;"
    "      recordBtn.textContent = 'Processing...';"
    "      result.style.background = '#e3f2fd';"
    "      result.textContent = 'Transcribing audio...';"
    "      stateText.textContent = 'Transcribing';"
    "      pollInterval = setInterval(pollStatus, 500);"
    "    } else if (data.state === 'done') {"
    "      recordBtn.style.display = 'none';"
    "      resetBtn.style.display = 'block';"
    "      result.style.background = '#e8f5e9';"
    "      result.textContent = data.transcription || '(No speech detected)';"
    "      stateText.textContent = 'Done';"
    "    } else if (data.state === 'error') {"
    "      result.style.background = '#ffebee';"
    "      result.textContent = 'Error: ' + (data.error || 'Unknown error');"
    "      stateText.textContent = 'Error';"
    "    }"
    "  } catch (err) { console.error('Initial state check error:', err); }"
    "}"
    "checkInitialState();"
    "</script>"
    "</body>"
    "</html>";

/* Live STT HTML page content */
static const char *LIVE_STT_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Live STT - ESP32-P4</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    COMMON_CSS
    "<style>"
    ".stream-btn { width: 100%; }"
    ".stream-btn.streaming { background: #27ae60; animation: pulse 1s infinite; }"
    "@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }"
    ".connection-status { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 8px; }"
    ".connection-status.disconnected { background: #bdc3c7; }"
    ".connection-status.connecting { background: #f39c12; animation: blink 1s infinite; }"
    ".connection-status.connected { background: #27ae60; }"
    "@keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }"
    "</style>"
    "</head>"
    "<body>"
    SIDEBAR_HTML(ACTIVE_LIVE)
    "<div class=\"main-content\">"
    "<div class=\"page-header\">"
    "<h1>Live Speech-to-Text</h1>"
    "<p class=\"subtitle\">Real-time transcription via Deepgram</p>"
    "</div>"
    "<div class=\"card\">"
    "<div class=\"status-bar\">"
    "<span class=\"connection-status disconnected\" id=\"connStatus\"></span>"
    "<strong>Status:</strong> <span id=\"stateText\">Idle</span>"
    "</div>"
    "<button id=\"streamBtn\" class=\"stream-btn success\">Start Streaming</button>"
    "<div style=\"margin-top:15px\">"
    "<button id=\"clearBtn\" class=\"secondary\" style=\"width:100%\">Clear Transcript</button>"
    "</div>"
    "<h3 style=\"margin-top:20px;margin-bottom:10px;color:#34495e\">Transcript</h3>"
    "<div id=\"transcript\" class=\"transcript-box\">Transcription will appear here in real-time...</div>"
    "</div>"
    "</div>"
    "<script>"
    "let isStreaming = false;"
    "let pollInterval = null;"
    "const streamBtn = document.getElementById('streamBtn');"
    "const clearBtn = document.getElementById('clearBtn');"
    "const transcript = document.getElementById('transcript');"
    "const stateText = document.getElementById('stateText');"
    "const connStatus = document.getElementById('connStatus');"
    "function updateUI(state, text) {"
    "  stateText.textContent = state;"
    "  connStatus.className = 'connection-status ' + (state === 'Streaming' ? 'connected' : state === 'Connecting' ? 'connecting' : 'disconnected');"
    "  if (text !== undefined) transcript.textContent = text || 'Transcription will appear here in real-time...';"
    "}"
    "async function startStreaming() {"
    "  try {"
    "    streamBtn.disabled = true;"
    "    updateUI('Connecting');"
    "    const resp = await fetch('/api/live/start', { method: 'POST' });"
    "    const data = await resp.json();"
    "    if (resp.ok) {"
    "      isStreaming = true;"
    "      streamBtn.textContent = 'Stop Streaming';"
    "      streamBtn.classList.remove('success');"
    "      streamBtn.classList.add('streaming', 'danger');"
    "      streamBtn.disabled = false;"
    "      updateUI('Streaming');"
    "      pollInterval = setInterval(pollTranscript, 300);"
    "    } else {"
    "      updateUI('Error');"
    "      transcript.textContent = 'Error: ' + (data.error || 'Failed to start');"
    "      streamBtn.disabled = false;"
    "    }"
    "  } catch (err) {"
    "    updateUI('Error');"
    "    transcript.textContent = 'Network error: ' + err.message;"
    "    streamBtn.disabled = false;"
    "  }"
    "}"
    "async function stopStreaming() {"
    "  clearInterval(pollInterval);"
    "  try {"
    "    await fetch('/api/live/stop', { method: 'POST' });"
    "  } catch (err) { console.error('Stop error:', err); }"
    "  isStreaming = false;"
    "  streamBtn.textContent = 'Start Streaming';"
    "  streamBtn.classList.remove('streaming', 'danger');"
    "  streamBtn.classList.add('success');"
    "  streamBtn.disabled = false;"
    "  updateUI('Idle');"
    "}"
    "async function pollTranscript() {"
    "  try {"
    "    const resp = await fetch('/api/live/status');"
    "    const data = await resp.json();"
    "    if (data.state === 'streaming') {"
    "      if (data.transcript) transcript.textContent = data.transcript;"
    "      transcript.scrollTop = transcript.scrollHeight;"
    "    } else if (data.state === 'error') {"
    "      clearInterval(pollInterval);"
    "      isStreaming = false;"
    "      streamBtn.textContent = 'Start Streaming';"
    "      streamBtn.classList.remove('streaming', 'danger');"
    "      streamBtn.classList.add('success');"
    "      streamBtn.disabled = false;"
    "      updateUI('Error');"
    "      transcript.textContent = 'Error: ' + (data.error || 'Connection lost');"
    "    } else if (data.state === 'idle') {"
    "      clearInterval(pollInterval);"
    "      isStreaming = false;"
    "      streamBtn.textContent = 'Start Streaming';"
    "      streamBtn.classList.remove('streaming', 'danger');"
    "      streamBtn.classList.add('success');"
    "      streamBtn.disabled = false;"
    "      updateUI('Idle');"
    "    }"
    "  } catch (err) { console.error('Poll error:', err); }"
    "}"
    "async function clearTranscript() {"
    "  try {"
    "    await fetch('/api/live/clear', { method: 'POST' });"
    "    transcript.textContent = 'Transcription will appear here in real-time...';"
    "  } catch (err) { console.error('Clear error:', err); }"
    "}"
    "streamBtn.addEventListener('click', function() {"
    "  if (isStreaming) { stopStreaming(); }"
    "  else { startStreaming(); }"
    "});"
    "clearBtn.addEventListener('click', clearTranscript);"
    "async function checkInitialState() {"
    "  try {"
    "    const resp = await fetch('/api/live/status');"
    "    const data = await resp.json();"
    "    if (data.state === 'streaming' || data.state === 'connecting') {"
    "      isStreaming = true;"
    "      streamBtn.textContent = 'Stop Streaming';"
    "      streamBtn.classList.remove('success');"
    "      streamBtn.classList.add('streaming', 'danger');"
    "      updateUI(data.state === 'streaming' ? 'Streaming' : 'Connecting', data.transcript);"
    "      pollInterval = setInterval(pollTranscript, 300);"
    "    } else if (data.transcript) {"
    "      transcript.textContent = data.transcript;"
    "    }"
    "  } catch (err) { console.error('Initial state check error:', err); }"
    "}"
    "checkInitialState();"
    "</script>"
    "</body>"
    "</html>";

/* OpenAI Live STT HTML page content */
static const char *OPENAI_LIVE_STT_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>OpenAI Live STT - ESP32-P4</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    COMMON_CSS
    "<style>"
    ".stream-btn { width: 100%; }"
    ".stream-btn.streaming { background: #27ae60; animation: pulse 1s infinite; }"
    "@keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }"
    ".connection-status { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 8px; }"
    ".connection-status.disconnected { background: #bdc3c7; }"
    ".connection-status.connecting { background: #f39c12; animation: blink 1s infinite; }"
    ".connection-status.connected { background: #27ae60; }"
    "@keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }"
    "</style>"
    "</head>"
    "<body>"
    SIDEBAR_HTML(ACTIVE_OPENAI)
    "<div class=\"main-content\">"
    "<div class=\"page-header\">"
    "<h1>Live Speech-to-Text (OpenAI)</h1>"
    "<p class=\"subtitle\">Real-time transcription via OpenAI Realtime API with Whisper</p>"
    "</div>"
    "<div class=\"card\">"
    "<div class=\"status-bar\">"
    "<span class=\"connection-status disconnected\" id=\"connStatus\"></span>"
    "<strong>Status:</strong> <span id=\"stateText\">Idle</span>"
    "</div>"
    "<button id=\"streamBtn\" class=\"stream-btn success\">Start Streaming</button>"
    "<div style=\"margin-top:15px\">"
    "<button id=\"clearBtn\" class=\"secondary\" style=\"width:100%\">Clear Transcript</button>"
    "</div>"
    "<h3 style=\"margin-top:20px;margin-bottom:10px;color:#34495e\">Transcript</h3>"
    "<div id=\"transcript\" class=\"transcript-box\">Transcription will appear here in real-time...</div>"
    "</div>"
    "</div>"
    "<script>"
    "let isStreaming = false;"
    "let pollInterval = null;"
    "const streamBtn = document.getElementById('streamBtn');"
    "const clearBtn = document.getElementById('clearBtn');"
    "const transcript = document.getElementById('transcript');"
    "const stateText = document.getElementById('stateText');"
    "const connStatus = document.getElementById('connStatus');"
    "function updateUI(state, text) {"
    "  stateText.textContent = state;"
    "  connStatus.className = 'connection-status ' + (state === 'Streaming' ? 'connected' : state === 'Connecting' ? 'connecting' : 'disconnected');"
    "  if (text !== undefined) transcript.textContent = text || 'Transcription will appear here in real-time...';"
    "}"
    "async function startStreaming() {"
    "  try {"
    "    streamBtn.disabled = true;"
    "    updateUI('Connecting');"
    "    const resp = await fetch('/api/openai-live/start', { method: 'POST' });"
    "    const data = await resp.json();"
    "    if (resp.ok) {"
    "      isStreaming = true;"
    "      streamBtn.textContent = 'Stop Streaming';"
    "      streamBtn.classList.remove('success');"
    "      streamBtn.classList.add('streaming', 'danger');"
    "      streamBtn.disabled = false;"
    "      updateUI('Streaming');"
    "      pollInterval = setInterval(pollTranscript, 300);"
    "    } else {"
    "      updateUI('Error');"
    "      transcript.textContent = 'Error: ' + (data.error || 'Failed to start');"
    "      streamBtn.disabled = false;"
    "    }"
    "  } catch (err) {"
    "    updateUI('Error');"
    "    transcript.textContent = 'Network error: ' + err.message;"
    "    streamBtn.disabled = false;"
    "  }"
    "}"
    "async function stopStreaming() {"
    "  clearInterval(pollInterval);"
    "  try {"
    "    await fetch('/api/openai-live/stop', { method: 'POST' });"
    "  } catch (err) { console.error('Stop error:', err); }"
    "  isStreaming = false;"
    "  streamBtn.textContent = 'Start Streaming';"
    "  streamBtn.classList.remove('streaming', 'danger');"
    "  streamBtn.classList.add('success');"
    "  streamBtn.disabled = false;"
    "  updateUI('Idle');"
    "}"
    "async function pollTranscript() {"
    "  try {"
    "    const resp = await fetch('/api/openai-live/status');"
    "    const data = await resp.json();"
    "    if (data.state === 'streaming') {"
    "      if (data.transcript) transcript.textContent = data.transcript;"
    "      transcript.scrollTop = transcript.scrollHeight;"
    "    } else if (data.state === 'error') {"
    "      clearInterval(pollInterval);"
    "      isStreaming = false;"
    "      streamBtn.textContent = 'Start Streaming';"
    "      streamBtn.classList.remove('streaming', 'danger');"
    "      streamBtn.classList.add('success');"
    "      streamBtn.disabled = false;"
    "      updateUI('Error');"
    "      transcript.textContent = 'Error: ' + (data.error || 'Connection lost');"
    "    } else if (data.state === 'idle') {"
    "      clearInterval(pollInterval);"
    "      isStreaming = false;"
    "      streamBtn.textContent = 'Start Streaming';"
    "      streamBtn.classList.remove('streaming', 'danger');"
    "      streamBtn.classList.add('success');"
    "      streamBtn.disabled = false;"
    "      updateUI('Idle');"
    "    }"
    "  } catch (err) { console.error('Poll error:', err); }"
    "}"
    "async function clearTranscript() {"
    "  try {"
    "    await fetch('/api/openai-live/clear', { method: 'POST' });"
    "    transcript.textContent = 'Transcription will appear here in real-time...';"
    "  } catch (err) { console.error('Clear error:', err); }"
    "}"
    "streamBtn.addEventListener('click', function() {"
    "  if (isStreaming) { stopStreaming(); }"
    "  else { startStreaming(); }"
    "});"
    "clearBtn.addEventListener('click', clearTranscript);"
    "async function checkInitialState() {"
    "  try {"
    "    const resp = await fetch('/api/openai-live/status');"
    "    const data = await resp.json();"
    "    if (data.state === 'streaming' || data.state === 'connecting') {"
    "      isStreaming = true;"
    "      streamBtn.textContent = 'Stop Streaming';"
    "      streamBtn.classList.remove('success');"
    "      streamBtn.classList.add('streaming', 'danger');"
    "      updateUI(data.state === 'streaming' ? 'Streaming' : 'Connecting', data.transcript);"
    "      pollInterval = setInterval(pollTranscript, 300);"
    "    } else if (data.transcript) {"
    "      transcript.textContent = data.transcript;"
    "    }"
    "  } catch (err) { console.error('Initial state check error:', err); }"
    "}"
    "checkInitialState();"
    "</script>"
    "</body>"
    "</html>";

/* Settings HTML page content */
static const char *SETTINGS_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>Settings - ESP32-P4</title>"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    COMMON_CSS
    "</head>"
    "<body>"
    SIDEBAR_HTML(ACTIVE_SETTINGS)
    "<div class=\"main-content\">"
    "<div class=\"page-header\">"
    "<h1>Settings</h1>"
    "<p class=\"subtitle\">System configuration and API status</p>"
    "</div>"
    "<div class=\"card\">"
    "<h2>Audio Settings</h2>"
    "<div class=\"control-group\">"
    "<label for=\"volume\">Volume:</label>"
    "<div class=\"slider-row\">"
    "<input type=\"range\" id=\"volume\" name=\"volume\" min=\"0\" max=\"100\" step=\"5\" value=\"80\">"
    "<span id=\"volumeVal\">80%</span>"
    "</div>"
    "</div>"
    "</div>"
    "<div class=\"card\">"
    "<h2>API Configuration Status</h2>"
    "<div id=\"apiStatus\">Loading...</div>"
    "</div>"
    "<div class=\"card\">"
    "<h2>System Information</h2>"
    "<div class=\"settings-row\">"
    "<span class=\"label\">Board</span>"
    "<span class=\"value\">Waveshare ESP32-P4-WIFI6-M</span>"
    "</div>"
    "<div class=\"settings-row\">"
    "<span class=\"label\">Processor</span>"
    "<span class=\"value\">ESP32-P4 + ESP32-C6 (WiFi)</span>"
    "</div>"
    "</div>"
    "</div>"
    "<script>"
    "const volumeSlider = document.getElementById('volume');"
    "const volumeVal = document.getElementById('volumeVal');"
    "volumeSlider.addEventListener('input', function() {"
    "  volumeVal.textContent = this.value + '%';"
    "});"
    "volumeSlider.addEventListener('change', async function() {"
    "  try {"
    "    await fetch('/api/volume', {"
    "      method: 'POST',"
    "      headers: { 'Content-Type': 'application/json' },"
    "      body: JSON.stringify({ volume: parseInt(this.value) })"
    "    });"
    "  } catch (err) { console.error('Volume error:', err); }"
    "});"
    "async function loadSettings() {"
    "  try {"
    "    const resp = await fetch('/api/settings');"
    "    const data = await resp.json();"
    "    let html = '';"
    "    data.apis.forEach(api => {"
    "      const statusClass = api.configured ? 'configured' : 'not-configured';"
    "      const statusText = api.configured ? 'Configured' : 'Not Configured';"
    "      html += '<div class=\"settings-row\">';"
    "      html += '<span class=\"label\">' + api.name + '</span>';"
    "      html += '<span class=\"value ' + statusClass + '\">' + statusText + '</span>';"
    "      html += '</div>';"
    "    });"
    "    document.getElementById('apiStatus').innerHTML = html;"
    "  } catch (err) {"
    "    document.getElementById('apiStatus').innerHTML = '<span class=\"value not-configured\">Failed to load</span>';"
    "  }"
    "}"
    "loadSettings();"
    "</script>"
    "</body>"
    "</html>";

/* Handler for root path "/" - TTS page */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving TTS page");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, TTS_HTML, strlen(TTS_HTML));
    return ESP_OK;
}

/* Handler for "/api/status" */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving status API");
    httpd_resp_set_type(req, "application/json");

    char json[256];
    snprintf(json, sizeof(json),
             "{\"status\":\"ok\",\"board\":\"ESP32-P4-WIFI6-M\",\"tts_provider\":\"%s\"}",
             tts_get_provider_name(tts_get_provider()));
    httpd_resp_send(req, json, strlen(json));
    return ESP_OK;
}

/* Handler for "/api/provider" GET - returns available providers */
static esp_err_t provider_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Provider GET API called");

    // Initialize TTS if not done yet to get provider availability
    if (!tts_initialized) {
        esp_err_t err = tts_init();
        if (err == ESP_OK) {
            tts_initialized = true;
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *providers = cJSON_CreateArray();

    // Add ElevenLabs
    cJSON *el = cJSON_CreateObject();
    cJSON_AddNumberToObject(el, "id", TTS_PROVIDER_ELEVENLABS);
    cJSON_AddStringToObject(el, "name", "ElevenLabs");
    cJSON_AddBoolToObject(el, "available", tts_is_provider_available(TTS_PROVIDER_ELEVENLABS));
    cJSON_AddItemToArray(providers, el);

    // Add OpenAI
    cJSON *oa = cJSON_CreateObject();
    cJSON_AddNumberToObject(oa, "id", TTS_PROVIDER_OPENAI);
    cJSON_AddStringToObject(oa, "name", "OpenAI");
    cJSON_AddBoolToObject(oa, "available", tts_is_provider_available(TTS_PROVIDER_OPENAI));
    cJSON_AddItemToArray(providers, oa);

    cJSON_AddItemToObject(root, "providers", providers);
    cJSON_AddNumberToObject(root, "current", tts_get_provider());

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/* Handler for "/api/provider" POST - sets the provider */
static esp_err_t provider_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Provider POST API called");

    // Initialize TTS if not done yet
    if (!tts_initialized) {
        esp_err_t err = tts_init();
        if (err != ESP_OK) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "500 Internal Server Error");
            const char *error = "{\"error\":\"TTS initialization failed\"}";
            httpd_resp_send(req, error, strlen(error));
            return ESP_OK;
        }
        tts_initialized = true;
    }

    // Read request body
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 256) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Invalid content length\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    char buf[257];
    int received = httpd_req_recv(req, buf, content_len);
    if (received <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Failed to read request body\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }
    buf[received] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Invalid JSON\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    cJSON *provider_item = cJSON_GetObjectItem(root, "provider");
    if (!provider_item || !cJSON_IsNumber(provider_item)) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Missing or invalid 'provider' field\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    int provider = provider_item->valueint;
    cJSON_Delete(root);

    // Set provider
    esp_err_t err = tts_set_provider((tts_provider_t)provider);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Provider not available (API key not configured)\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "TTS provider set to %s", tts_get_provider_name((tts_provider_t)provider));

    // Return success
    httpd_resp_set_type(req, "application/json");
    char response[128];
    snprintf(response, sizeof(response), "{\"provider\":%d,\"name\":\"%s\"}",
             provider, tts_get_provider_name((tts_provider_t)provider));
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/* TTS task - runs TTS with sufficient stack, signals completion */
typedef struct {
    char *text;
    float speed;
    SemaphoreHandle_t done_sem;
    esp_err_t result;
} tts_task_params_t;

static void tts_background_task(void *arg)
{
    tts_task_params_t *params = (tts_task_params_t *)arg;

    ESP_LOGI(TAG, "TTS background task: speaking '%.*s%s' at speed %.1fx",
             50, params->text, strlen(params->text) > 50 ? "..." : "", params->speed);

    params->result = tts_speak_with_speed(params->text, params->speed);
    if (params->result != ESP_OK) {
        ESP_LOGE(TAG, "TTS speak failed: %s", esp_err_to_name(params->result));
    }

    free(params->text);
    params->text = NULL;

    // Signal completion
    xSemaphoreGive(params->done_sem);
    vTaskDelete(NULL);
}

/* Handler for "/api/tts" POST */
static esp_err_t tts_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "TTS API called");

    // Initialize TTS on first call
    if (!tts_initialized) {
        esp_err_t err = tts_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TTS init failed: %s", esp_err_to_name(err));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "500 Internal Server Error");
            const char *error = "{\"error\":\"TTS initialization failed\"}";
            httpd_resp_send(req, error, strlen(error));
            return ESP_OK;
        }
        tts_initialized = true;
    }

    // Check if TTS is already playing
    if (tts_is_playing()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "409 Conflict");
        const char *error = "{\"error\":\"TTS is already speaking\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    // Read request body
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 8192) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Invalid content length\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    char *buf = malloc(content_len + 1);
    if (!buf) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char *error = "{\"error\":\"Memory allocation failed\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    int received = httpd_req_recv(req, buf, content_len);
    if (received <= 0) {
        free(buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Failed to read request body\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }
    buf[received] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Invalid JSON\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    cJSON *text_item = cJSON_GetObjectItem(root, "text");
    if (!text_item || !cJSON_IsString(text_item) || strlen(text_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Missing or empty 'text' field\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    // Get optional speed parameter
    float speed = 1.0f;
    cJSON *speed_item = cJSON_GetObjectItem(root, "speed");
    if (speed_item && cJSON_IsNumber(speed_item)) {
        speed = (float)speed_item->valuedouble;
        // Speed clamping is done in tts_speak_with_speed based on provider
    }

    // Copy text for background task
    char *text_copy = strdup(text_item->valuestring);
    cJSON_Delete(root);

    if (!text_copy) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char *error = "{\"error\":\"Memory allocation failed\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    // Create semaphore for completion notification
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinary();
    if (!done_sem) {
        free(text_copy);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char *error = "{\"error\":\"Memory allocation failed\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    // Create task params
    tts_task_params_t *params = malloc(sizeof(tts_task_params_t));
    if (!params) {
        free(text_copy);
        vSemaphoreDelete(done_sem);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char *error = "{\"error\":\"Memory allocation failed\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }
    params->text = text_copy;
    params->speed = speed;
    params->done_sem = done_sem;
    params->result = ESP_OK;

    // Calculate dynamic timeout based on text length and speed
    // ~12 chars/second at 1.0x speed, adjust for speed setting
    // Add 60 seconds buffer for download and processing overhead
    size_t text_len = strlen(text_copy);
    float effective_speed = (speed > 0.25f) ? speed : 1.0f;
    float chars_per_second = 12.0f * effective_speed;
    uint32_t estimated_duration_ms = (uint32_t)((text_len / chars_per_second) * 1000);
    uint32_t timeout_ms = estimated_duration_ms + 60000;  // Add 60s buffer
    if (timeout_ms < 30000) timeout_ms = 30000;  // Minimum 30 seconds
    ESP_LOGI(TAG, "TTS timeout set to %lu ms for %d chars at %.2fx speed",
             (unsigned long)timeout_ms, text_len, speed);

    // Start TTS in background task (needs large stack for HTTPS)
    BaseType_t task_created = xTaskCreate(
        tts_background_task,
        "tts_bg",
        16384,
        params,
        5,
        NULL
    );

    if (task_created != pdPASS) {
        free(text_copy);
        free(params);
        vSemaphoreDelete(done_sem);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char *error = "{\"error\":\"Failed to start TTS task\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    // Wait for TTS to complete with dynamic timeout
    if (xSemaphoreTake(done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG, "TTS timeout");
        tts_stop();
        vSemaphoreDelete(done_sem);
        free(params);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "504 Gateway Timeout");
        const char *error = "{\"error\":\"TTS timeout\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    // Get result and clean up
    esp_err_t tts_result = params->result;
    vSemaphoreDelete(done_sem);
    free(params);

    // Send response based on result
    httpd_resp_set_type(req, "application/json");
    if (tts_result == ESP_OK) {
        const char *success = "{\"status\":\"completed\"}";
        httpd_resp_send(req, success, strlen(success));
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char *error = "{\"error\":\"TTS playback failed\"}";
        httpd_resp_send(req, error, strlen(error));
    }
    return ESP_OK;
}

/* Handler for "/api/volume" POST */
static esp_err_t volume_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Volume API called");

    // Read request body
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 256) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Invalid content length\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    char buf[257];
    int received = httpd_req_recv(req, buf, content_len);
    if (received <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Failed to read request body\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }
    buf[received] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Invalid JSON\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    cJSON *volume_item = cJSON_GetObjectItem(root, "volume");
    if (!volume_item || !cJSON_IsNumber(volume_item)) {
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        const char *error = "{\"error\":\"Missing or invalid 'volume' field\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    int volume = volume_item->valueint;
    cJSON_Delete(root);

    // Clamp volume to valid range
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    // Set volume
    int volume_set;
    esp_err_t err = bsp_extra_codec_volume_set(volume, &volume_set);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set volume: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        const char *error = "{\"error\":\"Failed to set volume\"}";
        httpd_resp_send(req, error, strlen(error));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Volume set to %d", volume_set);

    // Return success
    httpd_resp_set_type(req, "application/json");
    char response[64];
    snprintf(response, sizeof(response), "{\"volume\":%d}", volume_set);
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/* Handler for GET /stt - STT page */
static esp_err_t stt_page_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving STT page");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, STT_HTML, strlen(STT_HTML));
    return ESP_OK;
}

/* Handler for POST /api/stt/start */
static esp_err_t stt_start_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "STT start API called");

    // Initialize STT on first call
    if (!stt_initialized) {
        esp_err_t err = stt_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "STT init failed: %s", esp_err_to_name(err));
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_status(req, "500 Internal Server Error");
            const char *error = "{\"error\":\"STT initialization failed. Check OpenAI API key.\"}";
            httpd_resp_send(req, error, strlen(error));
            return ESP_OK;
        }
        stt_initialized = true;
    }

    esp_err_t err = stt_start_recording();

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        const char *response = "{\"status\":\"recording\"}";
        httpd_resp_send(req, response, strlen(response));
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"error\":\"Failed to start recording: %s\"}", esp_err_to_name(err));
        httpd_resp_send(req, response, strlen(response));
    }
    return ESP_OK;
}

/* Handler for POST /api/stt/stop */
static esp_err_t stt_stop_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "STT stop API called");

    esp_err_t err = stt_stop_recording();

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        const char *response = "{\"status\":\"transcribing\"}";
        httpd_resp_send(req, response, strlen(response));
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"error\":\"Failed to stop recording: %s\"}", esp_err_to_name(err));
        httpd_resp_send(req, response, strlen(response));
    }
    return ESP_OK;
}

/* Handler for GET /api/stt/status */
static esp_err_t stt_status_handler(httpd_req_t *req)
{
    stt_status_t status;
    stt_get_status(&status);

    cJSON *root = cJSON_CreateObject();

    const char *state_str;
    switch (status.state) {
        case STT_STATE_IDLE: state_str = "idle"; break;
        case STT_STATE_RECORDING: state_str = "recording"; break;
        case STT_STATE_TRANSCRIBING: state_str = "transcribing"; break;
        case STT_STATE_DONE: state_str = "done"; break;
        case STT_STATE_ERROR: state_str = "error"; break;
        default: state_str = "unknown"; break;
    }
    cJSON_AddStringToObject(root, "state", state_str);

    if (status.transcription) {
        cJSON_AddStringToObject(root, "transcription", status.transcription);
    }
    if (status.error_message) {
        cJSON_AddStringToObject(root, "error", status.error_message);
    }
    cJSON_AddNumberToObject(root, "recording_ms", status.recording_ms);
    cJSON_AddNumberToObject(root, "audio_bytes", status.audio_bytes);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/* Handler for POST /api/stt/reset */
static esp_err_t stt_reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "STT reset API called");

    esp_err_t err = stt_reset();

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        const char *response = "{\"status\":\"idle\"}";
        httpd_resp_send(req, response, strlen(response));
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        char response[128];
        snprintf(response, sizeof(response),
                 "{\"error\":\"Failed to reset: %s\"}", esp_err_to_name(err));
        httpd_resp_send(req, response, strlen(response));
    }
    return ESP_OK;
}

/* Handler for GET /live - Live STT page */
static esp_err_t live_page_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving Live STT page");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, LIVE_STT_HTML, strlen(LIVE_STT_HTML));
    return ESP_OK;
}

/* Handler for GET /settings - Settings page */
static esp_err_t settings_page_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving Settings page");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SETTINGS_HTML, strlen(SETTINGS_HTML));
    return ESP_OK;
}

/* Handler for GET /api/settings - Get API configuration status */
static esp_err_t settings_api_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Settings API called");

    cJSON *root = cJSON_CreateObject();
    cJSON *apis = cJSON_CreateArray();

    // ElevenLabs
    cJSON *el = cJSON_CreateObject();
    cJSON_AddStringToObject(el, "name", "ElevenLabs TTS");
    cJSON_AddBoolToObject(el, "configured", tts_is_provider_available(TTS_PROVIDER_ELEVENLABS));
    cJSON_AddItemToArray(apis, el);

    // OpenAI
    cJSON *oa = cJSON_CreateObject();
    cJSON_AddStringToObject(oa, "name", "OpenAI TTS/Whisper");
    cJSON_AddBoolToObject(oa, "configured", tts_is_provider_available(TTS_PROVIDER_OPENAI));
    cJSON_AddItemToArray(apis, oa);

    // Deepgram
    cJSON *dg = cJSON_CreateObject();
    cJSON_AddStringToObject(dg, "name", "Deepgram Live STT");
#ifdef CONFIG_DEEPGRAM_API_KEY
    cJSON_AddBoolToObject(dg, "configured", strlen(CONFIG_DEEPGRAM_API_KEY) > 0);
#else
    cJSON_AddBoolToObject(dg, "configured", false);
#endif
    cJSON_AddItemToArray(apis, dg);

    // OpenAI Realtime (uses same OpenAI API key)
    cJSON *oai_rt = cJSON_CreateObject();
    cJSON_AddStringToObject(oai_rt, "name", "OpenAI Realtime Live STT");
    cJSON_AddBoolToObject(oai_rt, "configured", tts_is_provider_available(TTS_PROVIDER_OPENAI));
    cJSON_AddItemToArray(apis, oai_rt);

    cJSON_AddItemToObject(root, "apis", apis);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/* Handler for POST /api/live/start */
static esp_err_t live_start_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Live STT start API called");

    esp_err_t err = live_stt_start();

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        const char *response = "{\"status\":\"starting\"}";
        httpd_resp_send(req, response, strlen(response));
    } else if (err == ESP_ERR_INVALID_STATE) {
        // Check if it's because API key is not configured
        live_stt_status_t status;
        live_stt_get_status(&status);
        if (status.error_message && strstr(status.error_message, "API key")) {
            httpd_resp_set_status(req, "400 Bad Request");
            const char *response = "{\"error\":\"Deepgram API key not configured. Use 'idf.py menuconfig' to set it.\"}";
            httpd_resp_send(req, response, strlen(response));
        } else {
            httpd_resp_set_status(req, "409 Conflict");
            const char *response = "{\"error\":\"Already streaming\"}";
            httpd_resp_send(req, response, strlen(response));
        }
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char response[256];
        live_stt_status_t status;
        live_stt_get_status(&status);
        snprintf(response, sizeof(response),
                 "{\"error\":\"%s\"}", status.error_message ? status.error_message : "Failed to start streaming");
        httpd_resp_send(req, response, strlen(response));
    }
    return ESP_OK;
}

/* Handler for POST /api/live/stop */
static esp_err_t live_stop_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Live STT stop API called");

    live_stt_stop();

    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"stopped\"}";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/* Handler for GET /api/live/status */
static esp_err_t live_status_handler(httpd_req_t *req)
{
    live_stt_status_t status;
    live_stt_get_status(&status);

    cJSON *root = cJSON_CreateObject();

    const char *state_str;
    switch (status.state) {
        case LIVE_STT_STATE_IDLE: state_str = "idle"; break;
        case LIVE_STT_STATE_CONNECTING: state_str = "connecting"; break;
        case LIVE_STT_STATE_STREAMING: state_str = "streaming"; break;
        case LIVE_STT_STATE_ERROR: state_str = "error"; break;
        default: state_str = "unknown"; break;
    }
    cJSON_AddStringToObject(root, "state", state_str);

    if (status.transcript) {
        cJSON_AddStringToObject(root, "transcript", status.transcript);
    } else {
        cJSON_AddStringToObject(root, "transcript", "");
    }

    if (status.error_message) {
        cJSON_AddStringToObject(root, "error", status.error_message);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/* Handler for POST /api/live/clear */
static esp_err_t live_clear_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Live STT clear API called");

    live_stt_clear_transcript();

    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"cleared\"}";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/* Handler for GET /openai-live - OpenAI Live STT page */
static esp_err_t openai_live_page_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Serving OpenAI Live STT page");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, OPENAI_LIVE_STT_HTML, strlen(OPENAI_LIVE_STT_HTML));
    return ESP_OK;
}

/* Handler for POST /api/openai-live/start */
static esp_err_t openai_live_start_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OpenAI Live STT start API called");

    esp_err_t err = openai_live_stt_start();

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        const char *response = "{\"status\":\"starting\"}";
        httpd_resp_send(req, response, strlen(response));
    } else if (err == ESP_ERR_INVALID_STATE) {
        openai_live_stt_status_t status;
        openai_live_stt_get_status(&status);
        if (status.error_message && strstr(status.error_message, "API key")) {
            httpd_resp_set_status(req, "400 Bad Request");
            const char *response = "{\"error\":\"OpenAI API key not configured. Use 'idf.py menuconfig' to set it.\"}";
            httpd_resp_send(req, response, strlen(response));
        } else {
            httpd_resp_set_status(req, "409 Conflict");
            const char *response = "{\"error\":\"Already streaming\"}";
            httpd_resp_send(req, response, strlen(response));
        }
    } else {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char response[256];
        openai_live_stt_status_t status;
        openai_live_stt_get_status(&status);
        snprintf(response, sizeof(response),
                 "{\"error\":\"%s\"}", status.error_message ? status.error_message : "Failed to start streaming");
        httpd_resp_send(req, response, strlen(response));
    }
    return ESP_OK;
}

/* Handler for POST /api/openai-live/stop */
static esp_err_t openai_live_stop_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OpenAI Live STT stop API called");

    openai_live_stt_stop();

    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"stopped\"}";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/* Handler for GET /api/openai-live/status */
static esp_err_t openai_live_status_handler(httpd_req_t *req)
{
    openai_live_stt_status_t status;
    openai_live_stt_get_status(&status);

    cJSON *root = cJSON_CreateObject();

    const char *state_str;
    switch (status.state) {
        case OPENAI_LIVE_STT_STATE_IDLE: state_str = "idle"; break;
        case OPENAI_LIVE_STT_STATE_CONNECTING: state_str = "connecting"; break;
        case OPENAI_LIVE_STT_STATE_STREAMING: state_str = "streaming"; break;
        case OPENAI_LIVE_STT_STATE_ERROR: state_str = "error"; break;
        default: state_str = "unknown"; break;
    }
    cJSON_AddStringToObject(root, "state", state_str);

    if (status.transcript) {
        cJSON_AddStringToObject(root, "transcript", status.transcript);
    } else {
        cJSON_AddStringToObject(root, "transcript", "");
    }

    if (status.error_message) {
        cJSON_AddStringToObject(root, "error", status.error_message);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    return ESP_OK;
}

/* Handler for POST /api/openai-live/clear */
static esp_err_t openai_live_clear_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OpenAI Live STT clear API called");

    openai_live_stt_clear_transcript();

    httpd_resp_set_type(req, "application/json");
    const char *response = "{\"status\":\"cleared\"}";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

/* URI handlers */
static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_status = {
    .uri       = "/api/status",
    .method    = HTTP_GET,
    .handler   = status_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_provider_get = {
    .uri       = "/api/provider",
    .method    = HTTP_GET,
    .handler   = provider_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_provider_post = {
    .uri       = "/api/provider",
    .method    = HTTP_POST,
    .handler   = provider_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_tts = {
    .uri       = "/api/tts",
    .method    = HTTP_POST,
    .handler   = tts_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_volume = {
    .uri       = "/api/volume",
    .method    = HTTP_POST,
    .handler   = volume_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_stt_page = {
    .uri       = "/stt",
    .method    = HTTP_GET,
    .handler   = stt_page_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_stt_start = {
    .uri       = "/api/stt/start",
    .method    = HTTP_POST,
    .handler   = stt_start_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_stt_stop = {
    .uri       = "/api/stt/stop",
    .method    = HTTP_POST,
    .handler   = stt_stop_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_stt_status = {
    .uri       = "/api/stt/status",
    .method    = HTTP_GET,
    .handler   = stt_status_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_stt_reset = {
    .uri       = "/api/stt/reset",
    .method    = HTTP_POST,
    .handler   = stt_reset_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_live_page = {
    .uri       = "/live",
    .method    = HTTP_GET,
    .handler   = live_page_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_settings_page = {
    .uri       = "/settings",
    .method    = HTTP_GET,
    .handler   = settings_page_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_settings_api = {
    .uri       = "/api/settings",
    .method    = HTTP_GET,
    .handler   = settings_api_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_live_start = {
    .uri       = "/api/live/start",
    .method    = HTTP_POST,
    .handler   = live_start_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_live_stop = {
    .uri       = "/api/live/stop",
    .method    = HTTP_POST,
    .handler   = live_stop_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_live_status = {
    .uri       = "/api/live/status",
    .method    = HTTP_GET,
    .handler   = live_status_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_live_clear = {
    .uri       = "/api/live/clear",
    .method    = HTTP_POST,
    .handler   = live_clear_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_openai_live_page = {
    .uri       = "/openai-live",
    .method    = HTTP_GET,
    .handler   = openai_live_page_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_openai_live_start = {
    .uri       = "/api/openai-live/start",
    .method    = HTTP_POST,
    .handler   = openai_live_start_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_openai_live_stop = {
    .uri       = "/api/openai-live/stop",
    .method    = HTTP_POST,
    .handler   = openai_live_stop_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_openai_live_status = {
    .uri       = "/api/openai-live/status",
    .method    = HTTP_GET,
    .handler   = openai_live_status_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t uri_openai_live_clear = {
    .uri       = "/api/openai-live/clear",
    .method    = HTTP_POST,
    .handler   = openai_live_clear_handler,
    .user_ctx  = NULL
};

esp_err_t http_server_start(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 25;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI handlers */
    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_provider_get);
    httpd_register_uri_handler(server, &uri_provider_post);
    httpd_register_uri_handler(server, &uri_tts);
    httpd_register_uri_handler(server, &uri_volume);
    httpd_register_uri_handler(server, &uri_stt_page);
    httpd_register_uri_handler(server, &uri_stt_start);
    httpd_register_uri_handler(server, &uri_stt_stop);
    httpd_register_uri_handler(server, &uri_stt_status);
    httpd_register_uri_handler(server, &uri_stt_reset);
    httpd_register_uri_handler(server, &uri_live_page);
    httpd_register_uri_handler(server, &uri_settings_page);
    httpd_register_uri_handler(server, &uri_settings_api);
    httpd_register_uri_handler(server, &uri_live_start);
    httpd_register_uri_handler(server, &uri_live_stop);
    httpd_register_uri_handler(server, &uri_live_status);
    httpd_register_uri_handler(server, &uri_live_clear);
    httpd_register_uri_handler(server, &uri_openai_live_page);
    httpd_register_uri_handler(server, &uri_openai_live_start);
    httpd_register_uri_handler(server, &uri_openai_live_stop);
    httpd_register_uri_handler(server, &uri_openai_live_status);
    httpd_register_uri_handler(server, &uri_openai_live_clear);

    ESP_LOGI(TAG, "HTTP server started successfully");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (server == NULL) {
        ESP_LOGW(TAG, "Server not running");
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(server);
    if (ret == ESP_OK) {
        server = NULL;
        tts_initialized = false;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ret;
}
