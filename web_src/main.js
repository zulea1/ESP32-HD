/**
 * ============================================================================
 * File     : web_src/main.js
 * Project  : ESP32 HD - UDS Diagnostic Service Tester (ISO 14229 / ISO 15765-2)
 * Copyright: (c) 2026 Henzulea Daniel email: <zulea1@gmail.com>
 * License  : GNU General Public License v3.0 or later (GPL-3.0-or-later) - see LICENSE
 * Version  : @@FW_VERSION@@ (Build #@@FW_BUILD@@) - injected from src/config.h by build_web_ui.py
 * ============================================================================
 */

// Firmware identity: injected from src/config.h at build time, refreshed at runtime from /api/status.
let fwVersion = '@@FW_VERSION@@';
let fwBuild = '@@FW_BUILD@@';
function applyFwIdentity() {
  const fwStr = `${fwVersion} (Build #${fwBuild})`;
  const setTextById = (elementId, textValue) => { const element = document.getElementById(elementId); if (element) element.textContent = textValue; };
  setTextById('st-fw-badge', `FW: ${fwStr}`);
  setTextById('st-card-fw', `Build: ${fwStr}`);
  setTextById('bottom-version-tag', `v${fwVersion} build ${fwBuild}`);
}

// HD modules on the bus (9 Modules): SCM 0x7D0 (reply 0x7D8), 0x7E0..0x7E3,
// 0x7E5 and 0x7E7 (replies request+8) and 0x7F1/0x7F2.
// Display order (modules list, Display All, and canonical Search/Scan DTC ordering): ECM ABS BCM SCM SPDO RAD LHC RHC EHCU.
const ALL_HD_MODULES = [
  { id: 0x7E0, idHex: '0x7E0', acronym: 'ECM', descText: 'ECM (Engine Control Module)', isPrimary: true, responseId: '0x7E8' },
  { id: 0x7E1, idHex: '0x7E1', acronym: 'ABS', descText: 'ABS (Anti-lock Braking System)', isPrimary: true, responseId: '0x7E9' },
  { id: 0x7E2, idHex: '0x7E2', acronym: 'BCM', descText: 'BCM (Body Control Module)', isPrimary: true, responseId: '0x7EA' },
  { id: 0x7D0, idHex: '0x7D0', acronym: 'SCM', descText: 'SCM (Security Control Module)', isPrimary: true, responseId: '0x7D8' },
  { id: 0x7F2, idHex: '0x7F2', acronym: 'SPDO', descText: 'SPDO (Speedometer Module)', isPrimary: true, responseId: '0x7FA' },
  { id: 0x7F1, idHex: '0x7F1', acronym: 'RAD', descText: 'RAD (Radio / Infotainment Module)', isPrimary: true, responseId: '0x7F9' },
  { id: 0x7E3, idHex: '0x7E3', acronym: 'LHC', descText: 'LHC (Left Hand Controls)', isPrimary: true, responseId: '0x7EB' },
  { id: 0x7E5, idHex: '0x7E5', acronym: 'RHC', descText: 'RHC (Right Hand Controls)', isPrimary: true, responseId: '0x7ED' },
  { id: 0x7E7, idHex: '0x7E7', acronym: 'EHCU', descText: 'EHCU (Electro-Hydraulic Control Unit)', isPrimary: true, responseId: '0x7EF' }
];

// Re-orders a list of found modules (from /api/uds/search_modules) to match the canonical
// ALL_HD_MODULES order above, dropping nothing except modules that simply weren't found.
// Any module id the backend returns that isn't in ALL_HD_MODULES is kept, appended at the end.
function orderModulesCanonical(modules) {
  if (!Array.isArray(modules)) return modules;
  const hexOf = moduleEntry => (moduleEntry.idHex || moduleEntry.id_hex || ('0x' + Number(moduleEntry.id).toString(16).toUpperCase())).toUpperCase();
  const moduleByHex = {};
  modules.forEach(moduleEntry => { moduleByHex[hexOf(moduleEntry)] = moduleEntry; });
  const orderedModules = [];
  ALL_HD_MODULES.forEach(referenceModule => {
    const referenceHex = referenceModule.idHex.toUpperCase();
    if (moduleByHex[referenceHex]) { orderedModules.push(moduleByHex[referenceHex]); delete moduleByHex[referenceHex]; }
  });
  modules.forEach(moduleEntry => { if (moduleByHex[hexOf(moduleEntry)]) orderedModules.push(moduleEntry); });
  return orderedModules;
}

// Active State
let currentActiveModule = null;
let currentActiveOperation = null;
let currentWifiMode = 'STA';
let currentWifiRssi = -48;
let lastKnownDtcCount = -1; // -1: unread, 0: clean, >0: count
let moduleDtcCounts = {};   // Map of idHex -> number | 'scanning'
let moduleSelectPromise = null;
let isDiagOperationRunning = false;
let isReadingEcuInfo = false;
let ecuInfoRunId = 0;
let scanAllDtcRunId = 0;

// WebSocket & ECM (0x7E0) / ABS (0x7E1) / BCM (0x7E2) DID 0x0200 Live Stream State
let telemetryWs = null;
let ecmStreamActive = false;
let ecmStreamPollTimer = null;
let ecmStopPending = null;   // Promise of an in-flight HTTP stop request (ordering guard)
let ecmStartSeq = 0;         // Invalidates a delayed start if the user navigates away meanwhile
let ecmStreamFrameCount = 0;
let absStreamFrameCount = 0;
let bcmStreamFrameCount = 0;
let latestEcm0200Data = null;
let latestAbs0200Data = null;
let latestBcm0200Data = null;

function sleepMs(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

// ----------------------------------------------------------------------------
// Utilities: Clean ASCII Strings & Hex Fallback
// ----------------------------------------------------------------------------
function cleanAscii(str) {
  if (!str) return 'N/A';
  // Cut trailing dots '....', spaces, nulls, and non-printable chars
  let cleaned = str.replace(/[\x00-\x1F\x7F-\xFF]+$/g, '');
  cleaned = cleaned.replace(/\.+$/g, '').trim();
  
  if (cleaned.length === 0) return 'N/A';
  return cleaned;
}

// ----------------------------------------------------------------------------
// Main Screen: Search (0x7DF), Scan DTC (Detect + Read DTC on all), & Display All
// ----------------------------------------------------------------------------
function setTopScanModeActive(mode) {
  const searchButton = document.getElementById('btn-mode-search');
  const scanDtcButton = document.getElementById('btn-mode-scan-dtc');
  const allButton = document.getElementById('btn-mode-all');
  if (searchButton) searchButton.classList.toggle('active', mode === 'search');
  if (scanDtcButton) scanDtcButton.classList.toggle('active', mode === 'scan_dtc');
  if (allButton) allButton.classList.toggle('active', mode === 'all');
  // DTC lookup card belongs with Scan DTC only - hide it for Search / Display All (and any other mode).
  const dtcCard = document.getElementById('dtc-ref-card');
  if (dtcCard) dtcCard.style.display = (mode === 'scan_dtc') ? 'block' : 'none';
}

// LHC (0x7E3) and RHC (0x7E5) have no DTC table - Scan DTC skips them and
// their module-list card never shows a DTC badge; the Diagnostic Trouble Codes button (Read + Clear) is
// hidden from their dashboard too.
const MODULES_WITHOUT_DTC = ['0X7E3', '0X7E5'];
function moduleSupportsDtc(idHex) { return MODULES_WITHOUT_DTC.indexOf(String(idHex || '').toUpperCase()) === -1; }
// RHC (0x7E5) additionally has no special functions of its own. SCM (0x7D0) has neither special
// functions nor output tests - no actuator/output tests, no special functions - unlike ECM/ABS/BCM
// which support both.
const MODULES_WITHOUT_SPECIAL = ['0X7E5', '0X7D0'];
function moduleSupportsSpecial(idHex) { return MODULES_WITHOUT_SPECIAL.indexOf(String(idHex || '').toUpperCase()) === -1; }
// RAD (0x7F1) likewise: special functions yes, no output tests, no live data.
const MODULES_WITHOUT_OUTPUTS = ['0X7D0', '0X7F1', '0X7E3', '0X7E5'];
function moduleSupportsOutputs(idHex) { return MODULES_WITHOUT_OUTPUTS.indexOf(String(idHex || '').toUpperCase()) === -1; }
// RAD (0x7F1) has no live input data (no DS/0x0200 stream), so hide Read Module Inputs for it.
const MODULES_WITHOUT_INPUTS = ['0X7F1'];
function moduleSupportsInputs(idHex) { return MODULES_WITHOUT_INPUTS.indexOf(String(idHex || '').toUpperCase()) === -1; }

function buildDtcBadgeHtml(stateOrCount) {
  if (stateOrCount === undefined || stateOrCount === null) return '';
  if (stateOrCount === 'scanning') {
    return '<span class="dtc-count-badge scanning-dtc">Scanning...</span>';
  }
  const dtcCount = Number(stateOrCount);
  if (isNaN(dtcCount) || dtcCount < 0) return '';
  if (dtcCount === 0) {
    return '<span class="dtc-count-badge clean-dtc">0 DTC</span>';
  }
  return `<span class="dtc-count-badge has-dtc">${dtcCount} DTC${dtcCount > 1 ? 's' : ''}</span>`;
}

function updateModuleCardDtcBadge(idHex) {
  const wrap = document.getElementById(`mod-dtc-badge-${idHex}`);
  if (wrap) {
    wrap.innerHTML = buildDtcBadgeHtml(moduleDtcCounts[idHex]);
  }
}

function executeSearchModules() {
  scanAllDtcRunId++;
  setTopScanModeActive('search');
  
  const listEl = document.getElementById('modules-list');
  const emptyEl = document.getElementById('modules-empty-state');
  if (listEl) {
    listEl.innerHTML = '';
    listEl.classList.add('hidden');
  }
  if (emptyEl) {
    emptyEl.classList.remove('hidden');
    emptyEl.innerHTML = `
      <div class="scan-spinner"></div>
      <p class="empty-text">Scanning the CAN bus via broadcast (0x7DF)… Waiting up to 2 seconds for ECU responses.</p>
    `;
  }

  isDiagOperationRunning = true;
  fetch('/api/uds/module/exit', { method: 'POST' }).catch(() => {});

  const searchBtnLabel = document.getElementById('search-btn-label');
  if (searchBtnLabel) searchBtnLabel.textContent = 'Searching...';

  fetch('/api/uds/search_modules', { method: 'POST' })
    .then(response => response.json())
    .then(data => {
      isDiagOperationRunning = false;
      if (searchBtnLabel) searchBtnLabel.textContent = 'Search';
      if (data && data.ok && data.modules && data.modules.length > 0) {
        renderModulesList(orderModulesCanonical(data.modules));
      } else {
        renderNoModulesFound();
      }
    })
    .catch(() => {
      isDiagOperationRunning = false;
      if (searchBtnLabel) searchBtnLabel.textContent = 'Search';
      renderNoModulesFound();
    });
}

// Single-Button Operation: Detect all responding modules (0x7DF) & scan DTCs (0x19) for each
async function executeScanAllDtc() {
  const myRunId = ++scanAllDtcRunId;
  setTopScanModeActive('scan_dtc');

  const listEl = document.getElementById('modules-list');
  const emptyEl = document.getElementById('modules-empty-state');
  if (listEl) {
    listEl.innerHTML = '';
    listEl.classList.add('hidden');
  }
  if (emptyEl) {
    emptyEl.classList.remove('hidden');
    emptyEl.innerHTML = `
      <div class="scan-spinner"></div>
      <p class="empty-text">Detecting responding HD ECUs (0x7DF broadcast) and scanning DTCs across all active modules…</p>
    `;
  }

  isDiagOperationRunning = true;
  fetch('/api/uds/module/exit', { method: 'POST' }).catch(() => {});

  const scanDtcLabel = document.getElementById('scan-dtc-btn-label');
  if (scanDtcLabel) scanDtcLabel.textContent = 'Scanning...';

  try {
    const searchResp = await fetch('/api/uds/search_modules', { method: 'POST' });
    const searchData = await searchResp.json();
    if (myRunId !== scanAllDtcRunId) return;

    if (!searchData || !searchData.ok || !searchData.modules || searchData.modules.length === 0) {
      isDiagOperationRunning = false;
      if (scanDtcLabel) scanDtcLabel.textContent = 'Scan DTC';
      renderNoModulesFound();
      return;
    }

    const detectedModules = orderModulesCanonical(searchData.modules);
    detectedModules.forEach(moduleEntry => {
      const moduleHex = moduleEntry.idHex || moduleEntry.id_hex || ('0x' + Number(moduleEntry.id).toString(16).toUpperCase());
      if (moduleSupportsDtc(moduleHex)) moduleDtcCounts[moduleHex] = 'scanning';
      else delete moduleDtcCounts[moduleHex];
    });

    renderModulesList(detectedModules);

    // Sequentially scan DTCs on every responding module that actually has a DTC table, and update its badge
    for (let moduleIndex = 0; moduleIndex < detectedModules.length; moduleIndex++) {
      if (myRunId !== scanAllDtcRunId) return;
      const currentModule = detectedModules[moduleIndex];
      const moduleHex = currentModule.idHex || currentModule.id_hex || ('0x' + Number(currentModule.id).toString(16).toUpperCase());
      if (!moduleSupportsDtc(moduleHex)) continue;
      try {
        const dtcResp = await fetch(`/api/uds/read_dtc?id=${encodeURIComponent(moduleHex)}`, { method: 'POST' });
        const dtcData = await dtcResp.json();
        if (myRunId !== scanAllDtcRunId) return;
        if (dtcData && dtcData.ok) {
          const dtcCount = Array.isArray(dtcData.dtcs) ? dtcData.dtcs.length : (dtcData.count || 0);
          moduleDtcCounts[moduleHex] = dtcCount;
        } else {
          moduleDtcCounts[moduleHex] = 0;
        }
      } catch (readDtcError) {
        moduleDtcCounts[moduleHex] = 0;
      }
      updateModuleCardDtcBadge(moduleHex);
      if (moduleIndex < detectedModules.length - 1) {
        await sleepMs(60);
      }
    }
  } catch (err) {
    if (myRunId === scanAllDtcRunId) {
      renderNoModulesFound();
    }
  } finally {
    if (myRunId === scanAllDtcRunId) {
      isDiagOperationRunning = false;
      if (scanDtcLabel) scanDtcLabel.textContent = 'Scan DTC';
    }
  }
}

function renderNoModulesFound() {
  const listEl = document.getElementById('modules-list');
  const emptyEl = document.getElementById('modules-empty-state');
  if (listEl) {
    listEl.innerHTML = '';
    listEl.classList.add('hidden');
  }
  if (emptyEl) {
    emptyEl.classList.remove('hidden');
    emptyEl.innerHTML = `
      <div class="empty-icon-pulse">
        <svg class="icon icon-lg text-rose" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><line x1="15" y1="9" x2="9" y2="15"/><line x1="9" y1="9" x2="15" y2="15"/></svg>
      </div>
      <p class="empty-text"><strong style="color:#f43f5e;">No ECU modules responded.</strong> Broadcast scan (0x7DF) completed with 0 responses. Check CAN bus cable and verify motorcycle ignition is set to ON.</p>
    `;
  }
}

// On Display All: Directly display all physical HD ECU request addresses (0x7E0..0x7E7, 0x7F0..0x7F2)
function executeDisplayAllModules() {
  scanAllDtcRunId++;
  isDiagOperationRunning = false;
  const scanDtcLabel = document.getElementById('scan-dtc-btn-label');
  if (scanDtcLabel) scanDtcLabel.textContent = 'Scan DTC';
  const searchBtnLabel = document.getElementById('search-btn-label');
  if (searchBtnLabel) searchBtnLabel.textContent = 'Search';

  setTopScanModeActive('all');
  fetch('/api/uds/module/exit', { method: 'POST' }).catch(() => {});

  renderModulesList(ALL_HD_MODULES);
}

function renderModulesList(modules) {
  inputsResetCache();      // a new module search invalidates every earlier read
  const listEl = document.getElementById('modules-list');
  const emptyEl = document.getElementById('modules-empty-state');
  if (!listEl) return;

  listEl.innerHTML = '';
  if (!modules || modules.length === 0) {
    if (emptyEl) emptyEl.classList.remove('hidden');
    listEl.classList.add('hidden');
    return;
  }

  if (emptyEl) emptyEl.classList.add('hidden');
  listEl.classList.remove('hidden');

  // Render in the order the caller passed in - callers are responsible for ordering
  // (ALL_HD_MODULES / orderModulesCanonical already put modules in the canonical
  // ECM, ABS, BCM, SCM, SPDO, RAD, LHC, RHC, EHCU sequence), not CAN id order.
  modules.forEach(mod => {
    const card = document.createElement('article');
    card.className = 'module-card';
    const hex = mod.idHex || mod.id_hex || ('0x' + Number(mod.id).toString(16).toUpperCase());
    const acronym = mod.acronym || hex;
    const desc = mod.descText || mod.description || mod.name || hex;

    if (mod.dtc_count !== undefined && moduleDtcCounts[hex] === undefined && moduleSupportsDtc(hex)) {
      moduleDtcCounts[hex] = mod.dtc_count;
    }

    const badgeHtml = moduleSupportsDtc(hex) ? buildDtcBadgeHtml(moduleDtcCounts[hex]) : '';

    card.onclick = () => enterEcuModule(hex, acronym, desc);

    card.innerHTML = `
      <div class="card-left">
        <span class="card-name-line" style="font-size:13.5px;font-weight:700;color:#fff;">${desc}</span>
      </div>
      <div class="card-right" style="display:flex;align-items:center;gap:8px;margin-left:auto;">
        <span id="mod-dtc-badge-${hex}">${badgeHtml}</span>
        <svg class="icon icon-chevron icon-sm" viewBox="0 0 24 24"><polyline points="9 18 15 12 9 6"/></svg>
      </div>
    `;
    listEl.appendChild(card);
  });
}

// ----------------------------------------------------------------------------
// Navigation Level 1 -> Level 2: Enter Module Dashboard
// Upper field: only 3 letters abbreviation (or address if unknown). No 3E 00!
// Background Tester Present keepalive is handled autonomously by ESP32 FreeRTOS Core 0
// ----------------------------------------------------------------------------
function enterEcuModule(idHex, acronym, fullName) {
  scanAllDtcRunId++;
  inputsResetCache();
  currentActiveModule = {
    idHex: idHex,
    acronym: acronym || idHex,
    name: fullName || (acronym ? (acronym + ' Module') : idHex)
  };

  moduleSelectPromise = fetch('/api/uds/module/select', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: `id=${encodeURIComponent(idHex)}&name=${encodeURIComponent(currentActiveModule.acronym)}`
  }).then(() => sleepMs(60)).catch(() => {});

  document.getElementById('view-modules').classList.add('hidden');
  document.getElementById('view-operation').classList.add('hidden');
  document.getElementById('view-dashboard').classList.remove('hidden');

  const nameEl = document.getElementById('active-mod-name');
  if (nameEl) {
    nameEl.textContent = currentActiveModule.acronym;
  }
  const descEl = document.getElementById('active-mod-desc');
  if (descEl) {
    // currentActiveModule.name carries the "ACRONYM (Full Name)" form (e.g. "ECM (Engine Control Module)")
    // used elsewhere (module list card, the "System" row in Read ECU Information). The acronym is already
    // the big title right above this line, so here - on every module - show just the plain full name: no
    // repeated acronym, no parentheses.
    const fullName = currentActiveModule.name || '';
    const bracketMatch = fullName.match(/^[A-Z0-9]+\s*\(([^)]+)\)\s*$/);
    descEl.textContent = bracketMatch ? bracketMatch[1] : fullName;
  }

  // LHC/RHC have no DTC table - hide those dashboard buttons; RHC also has no
  // special functions of its own; SCM has neither output tests nor special functions (see notes above
  // MODULES_WITHOUT_SPECIAL / MODULES_WITHOUT_OUTPUTS).
  const hasDtc = moduleSupportsDtc(idHex);
  const hasSpecial = moduleSupportsSpecial(idHex);
  const hasOutputs = moduleSupportsOutputs(idHex);
  const hasInputs = moduleSupportsInputs(idHex);
  const readDtcBtn = document.getElementById('op-btn-read-dtc');
  const specialBtn = document.getElementById('op-btn-special');
  const outputsBtn = document.getElementById('op-btn-outputs');
  const inputsBtn = document.getElementById('op-btn-inputs');
  if (readDtcBtn) readDtcBtn.classList.toggle('hidden', !hasDtc);
  if (specialBtn) specialBtn.classList.toggle('hidden', !hasSpecial);
  if (outputsBtn) outputsBtn.classList.toggle('hidden', !hasOutputs);
  if (inputsBtn) inputsBtn.classList.toggle('hidden', !hasInputs);
}

// Exit Module -> Return to Main Screen (Stops keepalive in C++ backend)
function exitEcuModule() {
  stopEcmLiveStream0200();
  if (currentActiveOperation === 'outputs') {
    cleanupModuleOutputs();
  }
  ecuInfoRunId++;
  isReadingEcuInfo = false;
  isDiagOperationRunning = false;
  fetch('/api/uds/module/exit', { method: 'POST' }).catch(() => {});
  inputsResetCache();
  currentActiveModule = null;

  document.getElementById('view-dashboard').classList.add('hidden');
  document.getElementById('view-operation').classList.add('hidden');
  document.getElementById('view-modules').classList.remove('hidden');
}

// ----------------------------------------------------------------------------
// Navigation Level 2 -> Level 3: Dedicated Operation Sub-Page
// Upper field: only operation name. No 3E 00, no second small line!
// ----------------------------------------------------------------------------
function navigateToOperation(op) {
  if (op === 'inputs') inputsResetCache();     // never show a previous read when the page opens
  stopEcmLiveStream0200();
  if (currentActiveOperation === 'outputs' && op !== 'outputs') {
    cleanupModuleOutputs();
  }
  if (currentActiveOperation === 'special' && op !== 'special') ecmSpcExit(false);
  currentActiveOperation = op;
  ecuInfoRunId++;
  isReadingEcuInfo = false;

  document.getElementById('view-dashboard').classList.add('hidden');
  document.getElementById('view-operation').classList.remove('hidden');

  const titleEl = document.getElementById('op-page-title');
  const backBtnText = document.getElementById('op-back-btn-text');
  if (backBtnText && currentActiveModule) {
    backBtnText.textContent = currentActiveModule.acronym;
  }

  const container = document.getElementById('op-content-container');
  if (container) container.innerHTML = '<div class="results-card"><p class="text-dim">Loading operation...</p></div>';

  if (op === 'ecu_info') {
    if (titleEl) titleEl.textContent = 'Read ECU Information';
    executeReadEcuInfo();
  } else if (op === 'read_dtc') {
    if (titleEl) titleEl.textContent = 'Diagnostic Trouble Codes';
    executeReadDtc();
  } else if (op === 'inputs') {
    if (titleEl) titleEl.textContent = 'Read Module Inputs';
    renderModuleInputsView();
  } else if (op === 'outputs') {
    if (titleEl) titleEl.textContent = 'Set Module Outputs';
    renderModuleOutputsView();
  } else if (op === 'special') {
    if (titleEl) titleEl.textContent = 'Special Functions';
    renderSpecialFunctionsView();
  }
}

function exitOperationToDashboard() {
  stopEcmLiveStream0200();
  if (currentActiveOperation === 'outputs') {
    cleanupModuleOutputs();
  }
  if (currentActiveOperation === 'special') ecmSpcExit(false);
  currentActiveOperation = null;
  ecuInfoRunId++;
  isReadingEcuInfo = false;
  isDiagOperationRunning = false;
  document.getElementById('view-operation').classList.add('hidden');
  document.getElementById('view-dashboard').classList.remove('hidden');
}

// ----------------------------------------------------------------------------
// Operation 1: Read ECU Information
// Sequential DIDs: 0xF1EF (VIN), 0xF1EA (Parts Number), 0xF1ED (Calibration ID), 0xF1EE (Homologation ID)
// Reads each DID one-by-one with explicit verification & retry so no DID command is ever skipped or dropped
// ----------------------------------------------------------------------------
async function fetchSingleDidWithRetry(modIdHex, didHex) {
  const url = `/api/uds/read_did?id=${encodeURIComponent(modIdHex)}&did=${encodeURIComponent(didHex)}`;
  for (let attempt = 0; attempt < 2; attempt++) {
    try {
      const resp = await fetch(url, { method: 'POST', cache: 'no-store' });
      if (resp.status === 404) {
        return { unsupportedEndpoint: true };
      }
      if (resp.ok) {
        const data = await resp.json();
        if (data && data.ok && data.got && data.value && data.value !== 'N/A') {
          return { got: true, value: cleanAscii(data.value) };
        }
      }
    } catch (fetchError) {
      // Retry on transient socket contention
    }
    if (attempt === 0) {
      await sleepMs(80);
    }
  }
  return { got: false, value: 'N/A' };
}

async function executeReadEcuInfo() {
  const container = document.getElementById('op-content-container');
  if (!container || !currentActiveModule) return;
  if (isReadingEcuInfo) return;

  const myRunId = ++ecuInfoRunId;
  isReadingEcuInfo = true;
  isDiagOperationRunning = true;
  const targetModId = currentActiveModule.idHex;

  container.innerHTML = `
    <div class="results-card">
      <div class="results-header-row">
        <span class="results-title">ECU Identification</span>
      </div>
      <div class="ecu-table-wrap">
        <table class="ecu-info-table">
          <thead>
            <tr style="border-bottom:1px solid var(--border-main);background:rgba(255,255,255,0.03);">
              <th style="padding:8px 14px;text-align:left;font-size:11px;font-weight:700;color:var(--text-muted);text-transform:uppercase;letter-spacing:0.05em;border-right:1px solid var(--border-main);width:38%;">Description</th>
              <th style="padding:8px 12px;text-align:left;font-size:11px;font-weight:700;color:var(--text-muted);text-transform:uppercase;letter-spacing:0.05em;">Readed Value</th>
            </tr>
          </thead>
          <tbody id="ecu-info-body">
            <tr><td class="ecu-desc-cell">Identifying module...</td><td class="ecu-val-cell"></td></tr>
          </tbody>
        </table>
      </div>
      <div id="ecu-vehicle-block"></div>
      <button type="button" id="btn-read-ecu-again" class="btn-action-primary" onclick="executeReadEcuInfo()" disabled>
        <span id="btn-read-ecu-label">Reading...</span>
      </button>
    </div>
  `;

  try {
    if (moduleSelectPromise) {
      await moduleSelectPromise;
      moduleSelectPromise = null;
    }

    // 1) The firmware picks the item list from the module identification string.
    let plan = null;
    try {
      const planResponse = await fetch(`/api/ecu/info/plan?id=${encodeURIComponent(targetModId)}`, { method: 'POST', cache: 'no-store' });
      if (planResponse.status !== 404) plan = await planResponse.json();
    } catch (planFetchError) { plan = null; }
    if (myRunId !== ecuInfoRunId) return;

    if (plan && plan.ok && plan.items) {
      const infoBody = document.getElementById('ecu-info-body');
      if (infoBody) {
        infoBody.innerHTML = plan.items.map((itemLabel, itemIndex) => `
          <tr>
            <td class="ecu-desc-cell">${spcEsc(itemLabel)}</td>
            <td class="ecu-val-cell"><div id="info-val-${itemIndex}" class="input-table info-val">Waiting...</div></td>
          </tr>`).join('');
      }
      // 2) Read the items one by one.
      for (let itemIndex = 0; itemIndex < plan.items.length; itemIndex++) {
        if (myRunId !== ecuInfoRunId) return;
        const inputEl = document.getElementById('info-val-' + itemIndex);
        if (inputEl) inputEl.textContent = 'Reading...';
        let value = 'N/A';
        for (let attempt = 0; attempt < 2; attempt++) {
          try {
            const itemResponse = await fetch(`/api/ecu/info/item?id=${encodeURIComponent(targetModId)}&i=${itemIndex}`, { method: 'POST', cache: 'no-store' });
            const itemData = await itemResponse.json();
            if (itemData && itemData.ok) { value = itemData.value || 'N/A'; break; }
          } catch (itemFetchError) { /* retry once */ }
          await sleepMs(80);
        }
        if (myRunId !== ecuInfoRunId) return;
        if (inputEl) inputEl.textContent = value;
      }
    } else {
      // Module without a database list (or older firmware): classic four identification DIDs.
      const infoBody = document.getElementById('ecu-info-body');
      const identificationRows = [
        ['VIN', '0xF1EF'], ['Parts Number', '0xF1EA'], ['Calibration ID', '0xF1ED'], ['Homologation ID', '0xF1EE']
      ];
      if (infoBody) {
        infoBody.innerHTML = identificationRows.map((identificationRow, itemIndex) => `
          <tr><td class="ecu-desc-cell">${identificationRow[0]}</td>
          <td class="ecu-val-cell"><div id="info-val-${itemIndex}" class="input-table info-val">Waiting...</div></td></tr>`).join('');
      }
      for (let itemIndex = 0; itemIndex < identificationRows.length; itemIndex++) {
        if (myRunId !== ecuInfoRunId) return;
        const inputEl = document.getElementById('info-val-' + itemIndex);
        if (inputEl) inputEl.textContent = 'Reading...';
        const didResult = await fetchSingleDidWithRetry(targetModId, identificationRows[itemIndex][1]);
        if (myRunId !== ecuInfoRunId) return;
        if (inputEl) inputEl.textContent = didResult.value;
        await sleepMs(50);
      }
    }
    if (myRunId === ecuInfoRunId) await loadVehicleInfo(targetModId, myRunId);
  } catch (err) {
    if (myRunId === ecuInfoRunId) {
      fallbackEcuInfo();
    }
  } finally {
    if (myRunId === ecuInfoRunId) {
      isReadingEcuInfo = false;
      isDiagOperationRunning = false;
      const readAgainButton = document.getElementById('btn-read-ecu-again');
      const readAgainLabel = document.getElementById('btn-read-ecu-label');
      if (readAgainButton) readAgainButton.disabled = false;
      if (readAgainLabel) readAgainLabel.textContent = 'Read Again';
    }
  }
}

// Vehicle block (System / Manufacturer / Model / EngType / Year / Country / VIN), decoded in firmware from the VIN.
async function loadVehicleInfo(modId, runId) {
  const vehicleBlock = document.getElementById('ecu-vehicle-block');
  if (!vehicleBlock) return;
  vehicleBlock.innerHTML = '<div class="veh-card"><div class="veh-title">Vehicle</div><div class="veh-note">Reading VIN...</div></div>';
  let vinData = null;
  try {
    const vinResponse = await fetch('/api/ecu/vin?id=' + encodeURIComponent(modId), { method: 'POST', cache: 'no-store' });
    if (vinResponse.status !== 404) vinData = await vinResponse.json();
  } catch (vinFetchError) { vinData = null; }
  if (runId !== ecuInfoRunId) return;
  if (!vinData || !vinData.ok) { vehicleBlock.innerHTML = ''; return; }
  const systemName = (currentActiveModule && (currentActiveModule.name || currentActiveModule.shortName)) || '';
  const vehicleRows = [['System', systemName]];
  if (vinData.found) {
    vehicleRows.push(['Manufacturer', vinData.manufacturer], ['Model', vinData.model + (vinData.type ? ' (' + vinData.type + ')' : '')],
              ['EngType', vinData.engine], ['Year', vinData.year ? String(vinData.year) : (vinData.range || '')], ['Country', vinData.country]);
  } else {
    vehicleRows.push(['Model', 'VIN not in database']);
  }
  vehicleRows.push(['VIN', vinData.vin]);
  vehicleBlock.innerHTML = '<div class="veh-card"><div class="veh-title">Vehicle</div>' +
    vehicleRows.filter(vehicleRow => vehicleRow[1]).map(vehicleRow => `<div class="veh-row"><span>${spcEsc(vehicleRow[0])}</span><b>${spcEsc(vehicleRow[1])}</b></div>`).join('') + '</div>';
}

function fallbackEcuInfo() {
  document.querySelectorAll('#ecu-info-body .info-val').forEach(el => {
    if (el.textContent === 'Reading...' || el.textContent === 'Waiting...') el.textContent = 'N/A';
  });
}

// ----------------------------------------------------------------------------
// Operation 2: Diagnostic Trouble Codes (Read DTC + Clear DTC merged into one page, 2026-10-31 - Clear used
// to be its own dashboard operation/page; it's now the "Clear" button below the fault-code list here).
// Display codes in ASCII manner: e.g. B2206-00 (0x08), U0141-01 (0x88)
// ----------------------------------------------------------------------------
// Shared busy-state for the Read Again / Clear button pair on the Diagnostic Trouble Codes page - disables
// both while either a read or a clear is in flight (so they can't be fired at the same time) and swaps the
// busy one's label to show what it's doing.
function dtcSetActionButtonsState(opts) {
  opts = opts || {};
  const readBtn = document.getElementById('btn-read-again');
  const readLbl = document.getElementById('btn-read-again-lbl');
  const clearBtn = document.getElementById('btn-execute-clear');
  const clearLbl = document.getElementById('btn-execute-clear-lbl');
  if (readBtn) readBtn.disabled = !!opts.disableAll;
  if (clearBtn) clearBtn.disabled = !!opts.disableAll;
  if (readLbl) readLbl.textContent = opts.reading ? 'Reading...' : 'Read Again';
  if (clearLbl) clearLbl.textContent = opts.clearing ? 'Clearing...' : 'Clear';
}

function executeReadDtc() {
  const container = document.getElementById('op-content-container');
  if (!container || !currentActiveModule) return;

  isDiagOperationRunning = true;

  container.innerHTML = `
    <div class="results-card">
      <div class="results-header-row">
        <span class="results-title">Diagnostic Trouble Codes</span>
      </div>
      <div id="dtc-results-box" class="dtc-list">
        <p class="text-dim">Reading fault codes...</p>
      </div>
      <div id="dtc-clear-feedback" class="hidden" style="margin:12px 0 0;font-size:12.5px;font-weight:600;padding:10px 12px;border-radius:var(--radius-md);"></div>
      <div class="dtc-actions-row">
        <button type="button" class="btn-action-primary" id="btn-read-again" onclick="executeReadDtc()">
          <span id="btn-read-again-lbl">Read Again</span>
        </button>
        <button type="button" class="btn btn-danger" id="btn-execute-clear" onclick="executeClearDtcNow()">
          <span id="btn-execute-clear-lbl">Clear</span>
        </button>
      </div>
    </div>
  `;
  dtcSetActionButtonsState({ disableAll: true, reading: true });

  fetch(`/api/uds/read_dtc?id=${encodeURIComponent(currentActiveModule.idHex)}`, { method: 'POST' })
    .then(response => response.json())
    .then(data => {
      isDiagOperationRunning = false;
      dtcSetActionButtonsState({ disableAll: false });
      const dtcResultsBox = document.getElementById('dtc-results-box');
      if (!dtcResultsBox) return;

      if (data && data.ok && data.dtcs && data.dtcs.length > 0) {
        lastKnownDtcCount = data.dtcs.length;
        if (currentActiveModule) {
          moduleDtcCounts[currentActiveModule.idHex] = data.dtcs.length;
          updateModuleCardDtcBadge(currentActiveModule.idHex);
        }
        renderDtcList(data.dtcs);
      } else if (data && data.ok) {
        lastKnownDtcCount = 0;
        if (currentActiveModule) {
          moduleDtcCounts[currentActiveModule.idHex] = 0;
          updateModuleCardDtcBadge(currentActiveModule.idHex);
        }
        dtcResultsBox.innerHTML = `
          <div style="padding:14px;background:rgba(16,185,129,0.08);border:1px solid rgba(16,185,129,0.25);border-radius:var(--radius-md);">
            <p class="text-emerald font-semibold" style="color:#10b981;font-weight:700;">No fault codes stored in module memory.</p>
            <p style="font-size:11.5px;color:var(--text-muted);margin-top:4px;">ECU confirmed diagnostic memory is clean (0 active or confirmed DTCs).</p>
          </div>
        `;
      } else {
        renderNoEcuResponseDtc();
      }
    })
    .catch(() => {
      isDiagOperationRunning = false;
      dtcSetActionButtonsState({ disableAll: false });
      renderNoEcuResponseDtc();
    });
}

// ----------------------------------------------------------------------------
// Comprehensive HD DTC Database (Mirror of hd_dtc_db.h)
// ----------------------------------------------------------------------------
const HD_DTC_MAP = {
  // P-Codes: ECM (Engine Control Module / Powertrain)
  'P0031': 'Front O2 Sensor Heater Circuit Low / Short Ground',
  'P0032': 'Front O2 Sensor Heater Circuit High / Short Voltage',
  'P0051': 'Rear O2 Sensor Heater Circuit Low / Short Ground',
  'P0052': 'Rear O2 Sensor Heater Circuit High / Short Voltage',
  'P0106': 'MAP Sensor Performance / Range Error',
  'P0107': 'MAP Sensor Circuit Open / Low Voltage',
  'P0108': 'MAP Sensor Circuit High Voltage',
  'P0112': 'IAT Sensor Circuit Low Voltage',
  'P0113': 'IAT Sensor Circuit High Voltage',
  'P0117': 'Engine Temp (ET) Sensor Low Voltage',
  'P0118': 'Engine Temp (ET) Sensor High Voltage / Open',
  'P0120': 'TPS 1 Circuit Range / Performance Error',
  'P0122': 'TPS 1 Circuit Low Voltage',
  'P0123': 'TPS 1 Circuit High Voltage',
  'P0131': 'Front O2 Sensor Circuit Low (Lean)',
  'P0132': 'Front O2 Sensor Circuit High (Rich)',
  'P0134': 'Front O2 Sensor Inactive / Open Circuit',
  'P0151': 'Rear O2 Sensor Circuit Low (Lean)',
  'P0152': 'Rear O2 Sensor Circuit High (Rich)',
  'P0154': 'Rear O2 Sensor Inactive / Open Circuit',
  'P0220': 'TPS 2 Circuit Range / Performance Error',
  'P0222': 'TPS 2 Circuit Low Voltage',
  'P0223': 'TPS 2 Circuit High Voltage',
  'P0261': 'Front Fuel Injector Circuit Open / Low',
  'P0262': 'Front Fuel Injector High / Shorted to Voltage',
  'P0263': 'Rear Fuel Injector Circuit Open / Low',
  'P0264': 'Rear Fuel Injector High / Shorted to Voltage',
  'P0265': 'Front Cylinder Active Fuel Management Circuit',
  'P0266': 'Rear Cylinder Active Fuel Management Circuit',
  'P0371': 'Crankshaft Position (CKP) Too Many Pulses',
  'P0372': 'Crankshaft Position (CKP) Too Few Pulses',
  'P0373': 'Crankshaft Position (CKP) Intermittent Sync',
  'P0374': 'Crankshaft Position (CKP) Sensor Sync Error',
  'P0444': 'Evaporative Purge Solenoid Open Circuit',
  'P0445': 'Evaporative Purge Solenoid Shorted to Voltage',
  'P0501': 'Vehicle Speed Sensor (VSS) Range / Performance',
  'P0502': 'Vehicle Speed Sensor (VSS) Low Input',
  'P0503': 'Vehicle Speed Sensor (VSS) Intermittent Fault',
  'P0505': 'Idle Air Control (IAC) System Malfunction',
  'P0506': 'Idle Speed Control Lower Than Expected',
  'P0507': 'Idle Speed Control Higher Than Expected',
  'P0562': 'ECM Battery Voltage Low (<10.5V)',
  'P0563': 'ECM Battery Voltage High (>16.0V)',
  'P0572': 'Brake Switch 1 Circuit Low',
  'P0573': 'Brake Switch 1 Circuit High',
  'P0603': 'ECM Internal Keep-Alive Memory (KAM) Error',
  'P0605': 'ECM Internal ROM Memory Error',
  'P0606': 'ECM Internal Microprocessor Integrity Fault',
  'P0607': 'ECM Internal Microprocessor Performance Error',
  'P0641': '5V Sensor Reference Voltage 1 Out of Range',
  'P0651': '5V Sensor Reference Voltage 2 Out of Range',
  'P1001': 'System Relay Circuit Open / Short to Ground',
  'P1002': 'System Relay Circuit High / Short to 12V',
  'P1003': 'System Relay Contacts Open',
  'P1004': 'System Relay Contacts Closed / Stuck',
  'P1009': 'Incorrect Password (ECM Immobilizer Lockout)',
  'P1010': 'Missing Password (Security System Lock)',
  'P1270': 'TGS / Actuator Slew Rate Error',
  'P1351': 'Front Ignition Coil Open / Short to Ground',
  'P1352': 'Front Ignition Coil Short to 12V',
  'P1353': 'Front Cylinder Combustion Misfire Detected',
  'P1354': 'Rear Cylinder Combustion Misfire Detected',
  'P1355': 'Rear Ignition Coil Open / Short to Ground',
  'P1356': 'Rear Ignition Coil Short to 12V',
  'P1357': 'Front Cylinder Combustion Intermittent Misfire',
  'P1358': 'Rear Cylinder Combustion Intermittent Misfire',
  'P1475': 'Exhaust Actuator Position Error (Active Exhaust)',
  'P1477': 'Exhaust Actuator Short to Ground',
  'P1478': 'Exhaust Actuator Short to Voltage / Open',
  'P1501': 'Jiffy Stand Sensor Circuit Open',
  'P1502': 'Jiffy Stand Sensor Short to Ground',
  'P1510': 'Electronic Throttle Control (ETC) Limited Power',
  'P1511': 'Electronic Throttle Control (ETC) Power Management',
  'P1512': 'Electronic Throttle Control (ETC) Forced Idle',
  'P1514': 'Air Flow Fault During ETC Limp Home Mode',
  'P1600': 'ETC Internal Microprocessor Watchdog Error',
  'P1601': 'ECM Communication Bus Integrity Error',
  'P1607': 'ECM Internal Acceleration Calculation Fault',
  'P1655': 'Starter Disable Output Circuit Open / Short',
  'P2100': 'ETC Motor Driver Circuit Open',
  'P2101': 'ETC Motor Circuit Range / Performance',
  'P2102': 'ETC Motor Circuit Low Voltage / Short Ground',
  'P2103': 'ETC Motor Circuit High Voltage / Short 12V',
  'P2105': 'ETC System Forced Engine Shutdown',
  'P2107': 'ETC Throttle Actuator Internal Processor Error',
  'P2108': 'ETC Internal Control Module Throttle Performance',
  'P2119': 'ETC Throttle Body Mechanical / Return Spring Fault',
  'P2122': 'Twist Grip Sensor 1 (TGS1) Circuit Low',
  'P2123': 'Twist Grip Sensor 1 (TGS1) Circuit High',
  'P2127': 'Twist Grip Sensor 2 (TGS2) Circuit Low',
  'P2128': 'Twist Grip Sensor 2 (TGS2) Circuit High',
  'P2135': 'TPS 1 / TPS 2 Correlation Voltage Error',
  'P2138': 'Twist Grip Sensor (TGS 1/2) Correlation Error',
  'P2176': 'ETC Throttle Position Not Learned at Idle',
  'P2184': 'Engine Coolant / Head Temp Sensor Low',
  'P2185': 'Engine Coolant / Head Temp Sensor High',
  'P2300': 'Front Ignition Coil Primary Control Low',
  'P2301': 'Front Ignition Coil Primary Control High',
  'P2303': 'Rear Ignition Coil Primary Control Low',
  'P2304': 'Rear Ignition Coil Primary Control High',

  // B-Codes: BCM (Body Control Module / Lighting / Security / Switches)
  'B0563': 'Speedometer Battery Voltage High',
  'B1004': 'Low Fuel Lamp Output Open / Short to Ground',
  'B1005': 'Fuel Level Sender Circuit Open / High Resistance',
  'B1006': 'Accessory Line Overvoltage (TSM/BCM)',
  'B1007': 'Ignition Line Overvoltage',
  'B1008': 'Reset Switch Closed / Shorted to Ground',
  'B1101': 'Left Turn Switch Input Circuit Fault',
  'B1103': 'Left Hand Control Module (LHCM) Internal Error',
  'B1121': 'Left Turn Signal Output Open / Fault',
  'B1122': 'Right Turn Signal Output Open / Fault',
  'B1123': 'Left Turn Indicator Short to Ground',
  'B1124': 'Right Turn Indicator Short to Ground',
  'B1125': 'Right Turn Indicator Short to 12V',
  'B1126': 'Left Turn Indicator Short to 12V',
  'B1131': 'Alarm Siren Output Circuit Low / Short Ground',
  'B1132': 'Alarm Siren Output Circuit High / Short 12V',
  'B1134': 'Starter Relay Output Open / Short to Ground',
  'B1135': 'Starter Relay Output Short to Voltage',
  'B1141': 'Ignition Switch Circuit Open / Low Voltage Fault',
  'B1142': 'Security Antenna Internal Fault / No Fob Signal',
  'B1143': 'Security Antenna Ground Short Fault',
  'B1144': 'Security Antenna Open Circuit Fault',
  'B1145': 'Security Antenna Short to 12V',
  'B1151': 'Right Turn Switch Input Circuit Fault',
  'B1153': 'Right Hand Control Module (RHCM) Internal Error',
  'B1154': 'Clutch Switch Circuit Short to Ground',
  'B1155': 'Clutch Switch Circuit Open',
  'B1200': 'Fuel Level Sender Internal Fault',
  'B1210': 'Fuel Level Sender Circuit Open / High',
  'B1211': 'Fuel Level Sender Circuit Short to Ground',
  'B1300': 'Radio Power Voltage High (>16.0V)',
  'B1301': 'Radio Power Voltage Low (<10.0V)',
  'B1302': 'Radio Faceplate Home Button Stuck',
  'B1303': 'Radio Faceplate Favorite Button Stuck',
  'B1304': 'Radio Faceplate Nav Button Stuck',
  'B1305': 'Radio Faceplate Power/Mute Button Stuck',
  'B1308': 'Front Left Speaker Circuit Open / High Resistance',
  'B1310': 'Front Right Speaker Circuit Open / High Resistance',
  'B1314': 'Rear Left Speaker Circuit Open / High Resistance',
  'B1318': 'Rear Right Speaker Circuit Open / High Resistance',
  'B1322': 'Front Left Speaker Short to Ground',
  'B1326': 'Front Right Speaker Short to Ground',
  'B1330': 'Rear Left Speaker Short to Ground',
  'B1334': 'Rear Right Speaker Short to Ground',
  'B1338': 'Front Left Speaker Short to Voltage',
  'B1342': 'Front Right Speaker Short to Voltage',
  'B1346': 'Rear Left Speaker Short to Voltage',
  'B1350': 'Rear Right Speaker Short to Voltage',
  'B1354': 'Radio Internal Microprocessor Comm Error',
  'B1401': 'Front Left Headset Microphone Open',
  'B1402': 'Front Left Headset Microphone Short',
  'B1403': 'Rear Right Headset Microphone Open',
  'B1404': 'Rear Right Headset Microphone Short',
  'B2102': 'System Power Output 1 Short to Ground',
  'B2103': 'System Power Output 1 Short to Voltage',
  'B2104': 'System Power Output 2 Overloaded',
  'B2106': 'System Power Output 3 Circuit Open',
  'B2107': 'System Power Output 3 Short to Ground',
  'B2108': 'System Power Output 3 Short to Voltage',
  'B2109': 'System Power Output 3 Overloaded',
  'B2112': 'Accessory Output Short to Ground / Open',
  'B2113': 'Accessory Output Short to Voltage',
  'B2114': 'Accessory Output Overloaded',
  'B2116': 'Fuel Pump Output Open Circuit',
  'B2117': 'Fuel Pump Output Short to Ground',
  'B2118': 'Fuel Pump Output Short to Voltage',
  'B2119': 'Fuel Pump Output Overloaded',
  'B2121': 'Starter Solenoid Output Open Circuit',
  'B2122': 'Starter Solenoid Output Short to Ground',
  'B2123': 'Starter Solenoid Output Short to Voltage',
  'B2124': 'Starter Solenoid Output Overloaded',
  'B2126': 'Horn Output Circuit Open',
  'B2127': 'Horn Output Short to Ground',
  'B2128': 'Horn Output Short to Voltage',
  'B2129': 'Horn Output Overloaded',
  'B2131': 'Headlamp High Beam Output Open',
  'B2132': 'Headlamp High Beam Output Short to Ground',
  'B2133': 'Headlamp High Beam Output Short to Voltage',
  'B2136': 'Headlamp Low Beam Output Open',
  'B2137': 'Headlamp Low Beam Output Short to Ground',
  'B2138': 'Headlamp Low Beam Output Short to Voltage',
  'B2141': 'Left Front Turn Signal Output Open',
  'B2142': 'Left Front Turn Signal Output Short to Ground',
  'B2146': 'Right Front Turn Signal Output Open',
  'B2147': 'Right Front Turn Signal Output Short to Ground',
  'B2151': 'Left Rear Turn Signal Output Open',
  'B2152': 'Left Rear Turn Signal Output Short to Ground',
  'B2156': 'Right Rear Turn Signal Output Open',
  'B2157': 'Right Rear Turn Signal Output Short to Ground',
  'B2161': 'Brake Lamp Output Open Circuit',
  'B2162': 'Brake Lamp Output Short to Ground',
  'B2163': 'Brake Lamp Output Short to Voltage',
  'B2164': 'Brake Lamp Output Overloaded',
  'B2168': 'Running Lights Output Open Circuit',
  'B2169': 'Running Lights Output Short to Ground',
  'B2172': 'Accessory H2 Output Circuit Open',
  'B2173': 'Accessory H2 Output Short to Ground',
  'B2176': 'Security Siren Output Circuit Open',
  'B2177': 'Security Siren Output Short to Ground',
  'B2178': 'Security Siren Output Short to Voltage',
  'B2201': 'Ignition Switch Off with Speed Sensor Active',
  'B2203': 'Ignition Switch Input Short to Ground',
  'B2206': 'Engine Stop / Run Switch Input Fault',
  'B2208': 'Engine Stop Switch Input Shorted',
  'B2210': 'Run / Stop Switch Circuit Open',
  'B2212': 'Run / Stop Switch Short to Ground',
  'B2218': 'Neutral Switch Input Short to Ground',
  'B2223': 'Rear Brake Switch Input Short to Ground',
  'B2226': 'Bank Angle Sensor (BAS) Input Open',
  'B2228': 'Bank Angle Sensor (BAS) Short to Ground',
  'B2250': 'Clutch Switch Stuck Active',
  'B2251': 'Horn Switch Stuck Active',
  'B2252': 'Left Turn Switch Stuck Active',
  'B2253': 'High Beam Flash Switch Stuck Active',
  'B2254': 'Right Turn Switch Stuck Active',
  'B2255': 'Trip Odometer Switch Stuck Active',
  'B2260': 'Start Switch Stuck Active',
  'B2261': 'Front Brake Switch Stuck Active',
  'B2262': 'Hazard Warning Switch Stuck Active',
  'B2263': 'Hazard Light Circuit Shorted to Ground',
  'B2264': 'Hazard Light Circuit Shorted to Voltage',
  'B2270': 'BCM Internal Hardware Fault',
  'B2271': 'BCM Battery Voltage Low (<10.0V)',
  'B2272': 'BCM Battery Voltage High (>16.0V)',
  'B2274': 'Constant Battery Line Circuit Fault',

  // C-Codes: ABS (Anti-lock Braking System / Chassis)
  'C0562': 'ABS Module Battery Voltage Low',
  'C0563': 'ABS Module Battery Voltage High',
  'C1014': 'ABS ECU Main Relay Malfunction',
  'C1015': 'ABS Valve Voltage Range Fault',
  'C1016': 'Brake Light Switch Always Active / Stuck Closed',
  'C1017': 'ABS Pump Motor Power Interrupted',
  'C1018': 'ABS System Operating Voltage Out of Range',
  'C1021': 'Front Wheel Speed Sensor Signal Zero',
  'C1023': 'Rear Wheel Speed Sensor Signal Zero',
  'C1025': 'Front Wheel Speed Sensor Intermittent / Noisy',
  'C1027': 'Rear Wheel Speed Sensor Intermittent / Noisy',
  'C1029': 'Front / Rear Wheel Speed Difference Too High',
  'C1032': 'Front Wheel Speed Sensor Circuit Open / Short',
  'C1034': 'Rear Wheel Speed Sensor Circuit Open / Short',
  'C1035': 'Front Brake Fluid Pressure Sensor Signal Fault',
  'C1040': 'ABS Pump Motor Circuit Malfunction',
  'C1041': 'Front Apply Solenoid Circuit Open / High Resistance',
  'C1042': 'Front Release Solenoid Circuit Open / High Resistance',
  'C1043': 'Rear Apply Solenoid Circuit Open / High Resistance',
  'C1044': 'Rear Release Solenoid Circuit Open / High Resistance',
  'C1045': 'ABS Hydraulic Valve Circuit Low / Ground Fault',
  'C1051': 'Front Brake Actuator Position Error',
  'C1052': 'Rear Brake Actuator Position Error',
  'C1055': 'ABS ECU Internal Microprocessor Fault',
  'C1061': 'ABS Front Apply Valve Circuit Fault',
  'C1062': 'ABS Front Release Valve Circuit Fault',
  'C1065': 'ABS Rear Apply Valve Circuit Fault',
  'C1066': 'ABS Rear Release Valve Circuit Fault',
  'C1071': 'ABS Internal Pressure Relief Valve Error',
  'C1072': 'ABS Rear Actuator Pressure Sensor Fault',
  'C1073': 'Front Brake Master Cylinder Pressure Sensor',
  'C1074': 'Rear Brake Master Cylinder Pressure Sensor',
  'C1075': 'Front Caliper Hydraulic Pressure Sensor Fault',
  'C1076': 'Rear Caliper Hydraulic Pressure Sensor Fault',
  'C1077': 'Linked Braking System Proportional Valve Error',
  'C1078': 'ABS Pump Motor Speed Signal Inconsistent',
  'C1081': 'Front Wheel Speed Sensor Tone Ring Teeth Missing',
  'C1082': 'Rear Wheel Speed Sensor Tone Ring Teeth Missing',
  'C1083': 'Front Brake Pad Wear Sensor Signal Active',
  'C1084': 'Rear Brake Pad Wear Sensor Signal Active',
  'C1085': 'Front Brake Hydraulic Pressure Mismatch',
  'C1086': 'Rear Brake Hydraulic Pressure Mismatch',
  'C1087': 'ABS Module Flash Memory CRC Checksum Error',
  'C1088': 'ABS Module RAM Memory Read/Write Error',
  'C1089': 'ABS Module Sensor Supply Voltage Out of Range',
  'C1158': 'ABS ECU Calibration / Configuration Error',
  'C1184': 'ABS System Disabled by Internal Diagnostic Fault',
  'C1250': 'ABS Modulator Unit Internal Seal Leak',

  // U-Codes: CAN Network Communication (Loss of Comm)
  'U0001': 'High-Speed CAN Bus Fault (Bus Off)',
  'U0002': 'High-Speed CAN Bus Performance Error',
  'U0003': 'CAN Bus High Circuit Open',
  'U0011': 'CAN Bus Low Circuit Shorted to Ground',
  'U0100': 'Lost Communication with ECM (Engine Control Module)',
  'U0110': 'Lost Communication with Drive Motor Control Module',
  'U0115': 'Lost Communication with ECM / Powertrain Control Module',
  'U0121': 'Lost Communication with ABS (Anti-lock Brake Module)',
  'U0122': 'Lost Communication with Vehicle Dynamics Control',
  'U0140': 'Lost Communication with BCM (Body Control Module)',
  'U0141': 'Lost Communication with LHCM (Left Hand Controls)',
  'U0142': 'Lost Communication with RHCM (Right Hand Controls)',
  'U0146': 'Lost Communication with Gateway Module',
  'U0155': 'Lost Communication with Instrument Panel Cluster',
  'U0156': 'Lost Communication with Speedometer / Instruments',
  'U0159': 'Lost Communication with Parking Assist / Sensor Module',
  'U0160': 'Lost Communication with Security Alarm Siren (HFSM)',
  'U0184': 'Lost Communication with Radio / Infotainment (RAD)',
  'U0185': 'Lost Communication with Front Audio Amplifier',
  'U0186': 'Lost Communication with Rear Audio Amplifier',
  'U0401': 'Invalid Data Received from ECM',
  'U0415': 'Invalid Data Received from ABS',
  'U0422': 'Invalid Data Received from BCM',
  'U1016': 'Loss of ECM Serial Data (CAN / J1850)',
  'U1040': 'Loss of ABS Serial Data (CAN)',
  'U1064': 'Loss of BCM / TSM Serial Data',
  'U1097': 'Loss of Speedometer Serial Data',
  'U1255': 'Serial Data Bus Missing Message (Bus Malfunction)',
  'U1300': 'Serial Data Bus Low / Short to Ground',
  'U1301': 'Serial Data Bus High / Short to 12V'
};

function interpretHDDtc(rawCode) {
  if (!rawCode) return 'HD Diagnostic Trouble Code';
  const clean = rawCode.replace(/[-_\s].*$/, '').trim().toUpperCase();
  if (HD_DTC_MAP[clean]) {
    return HD_DTC_MAP[clean];
  }
  const prefix = clean.charAt(0);
  if (prefix === 'P') return 'HD Powertrain / Engine Fault';
  if (prefix === 'B') return 'HD Body / Electrical / Switch Fault';
  if (prefix === 'C') return 'HD ABS / Chassis Brake Fault';
  if (prefix === 'U') return 'HD CAN Bus Network Communication Loss';
  return 'HD Diagnostic Trouble Code';
}

function renderNoEcuResponseDtc() {
  const dtcResultsBox = document.getElementById('dtc-results-box');
  if (!dtcResultsBox) return;
  lastKnownDtcCount = -1;
  const modStr = currentActiveModule ? `${currentActiveModule.acronym} (${currentActiveModule.idHex})` : 'module';
  dtcResultsBox.innerHTML = `
    <div style="padding:16px;background:rgba(244,63,94,0.08);border:1px solid rgba(244,63,94,0.25);border-radius:var(--radius-md);text-align:center;">
      <div class="empty-icon-pulse" style="margin-bottom:8px;">
        <svg class="icon icon-lg text-rose" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
      </div>
      <p class="text-rose font-semibold" style="color:#f43f5e;font-weight:700;font-size:13.5px;">No ECU Response</p>
      <p style="font-size:11.5px;color:var(--text-muted);margin-top:4px;">No reply received from ${modStr} on CAN bus. Verify diagnostic harness connection and ensure ignition switch is set to IGN/RUN.</p>
    </div>
  `;
}

function renderDtcList(dtcs) {
  const dtcResultsBox = document.getElementById('dtc-results-box');
  if (!dtcResultsBox) return;

  dtcResultsBox.innerHTML = '';
  dtcs.forEach(dtcEntry => {
    const desc = (dtcEntry.description && dtcEntry.description !== 'HD Diagnostic Trouble Code' && dtcEntry.description !== 'Diagnostic Trouble Code')
      ? dtcEntry.description
      : interpretHDDtc(dtcEntry.code);

    const dtcItemElement = document.createElement('div');
    dtcItemElement.className = 'dtc-item-card';
    dtcItemElement.innerHTML = `
      <span class="dtc-code-tag">${dtcEntry.code}</span>
      <div class="dtc-details">
        <div class="dtc-desc">${desc}</div>
        <div class="dtc-status-line">${dtcEntry.status_desc || 'Status: Active'}</div>
      </div>
    `;
    dtcResultsBox.appendChild(dtcItemElement);
  });
}

// ----------------------------------------------------------------------------
// Operation 3 (merged into the Diagnostic Trouble Codes page, 2026-10-31): Clear DTC. Transmits UDS Service
// 0x14 (04 14 FF FF FF) to erase stored trouble codes, freeze frames and reset DTC statuses, then updates the
// fault-code list and the module-card badge in place - no page switch, no re-fetch of the list from the ECU
// (we already know the outcome from the clear response and the count read moments earlier).
// ----------------------------------------------------------------------------
function executeClearDtcNow() {
  if (!currentActiveModule) return;
  const feedbackBox = document.getElementById('dtc-clear-feedback');
  dtcSetActionButtonsState({ disableAll: true, clearing: true });
  isDiagOperationRunning = true;

  fetch(`/api/uds/clear_dtc?id=${encodeURIComponent(currentActiveModule.idHex)}`, { method: 'POST' })
    .then(response => response.json())
    .then(data => {
      isDiagOperationRunning = false;
      dtcSetActionButtonsState({ disableAll: false });
      if (!feedbackBox) return;
      feedbackBox.classList.remove('hidden');

      if (data && data.ok) {
        if (currentActiveModule) {
          moduleDtcCounts[currentActiveModule.idHex] = 0;
          updateModuleCardDtcBadge(currentActiveModule.idHex);
        }
        const dtcResultsBox = document.getElementById('dtc-results-box');
        if (dtcResultsBox) {
          dtcResultsBox.innerHTML = `
            <div style="padding:14px;background:rgba(16,185,129,0.08);border:1px solid rgba(16,185,129,0.25);border-radius:var(--radius-md);">
              <p class="text-emerald font-semibold" style="color:#10b981;font-weight:700;">No fault codes stored in module memory.</p>
              <p style="font-size:11.5px;color:var(--text-muted);margin-top:4px;">ECU confirmed diagnostic memory is clean (0 active or confirmed DTCs).</p>
            </div>
          `;
        }
        if (lastKnownDtcCount === 0) {
          feedbackBox.style.background = 'rgba(56,189,248,0.12)';
          feedbackBox.style.border = '1px solid rgba(56,189,248,0.3)';
          feedbackBox.style.color = '#38bdf8';
          feedbackBox.textContent = `Nothing to clear: Diagnostic memory was already clean in ${currentActiveModule.acronym} (${currentActiveModule.idHex}).`;
        } else if (lastKnownDtcCount > 0) {
          feedbackBox.style.background = 'rgba(16,185,129,0.12)';
          feedbackBox.style.border = '1px solid rgba(16,185,129,0.3)';
          feedbackBox.style.color = '#10b981';
          feedbackBox.textContent = `Successfully cleared ${lastKnownDtcCount} trouble code${lastKnownDtcCount > 1 ? 's' : ''} from ${currentActiveModule.acronym} (${currentActiveModule.idHex}). Service 0x14 confirmed.`;
          lastKnownDtcCount = 0;
        } else {
          feedbackBox.style.background = 'rgba(16,185,129,0.12)';
          feedbackBox.style.border = '1px solid rgba(16,185,129,0.3)';
          feedbackBox.style.color = '#10b981';
          feedbackBox.textContent = `Trouble codes cleared successfully in ${currentActiveModule.acronym} (${currentActiveModule.idHex}).`;
        }
      } else {
        feedbackBox.style.background = 'rgba(244,63,94,0.12)';
        feedbackBox.style.border = '1px solid rgba(244,63,94,0.3)';
        feedbackBox.style.color = '#f43f5e';
        feedbackBox.textContent = `Clear Failed: No ECU Response. Module ${currentActiveModule.acronym} (${currentActiveModule.idHex}) did not acknowledge Service 0x14 on CAN bus.`;
      }
    })
    .catch(() => {
      isDiagOperationRunning = false;
      dtcSetActionButtonsState({ disableAll: false });
      if (!feedbackBox) return;
      feedbackBox.classList.remove('hidden');
      feedbackBox.style.background = 'rgba(244,63,94,0.12)';
      feedbackBox.style.border = '1px solid rgba(244,63,94,0.3)';
      feedbackBox.style.color = '#f43f5e';
      feedbackBox.textContent = `Clear Failed: No ECU Response. Module ${currentActiveModule.acronym} (${currentActiveModule.idHex}) did not acknowledge Service 0x14 on CAN bus.`;
    });
}

// ----------------------------------------------------------------------------
// Operation 4: Read Module Inputs (ECM DID 0x0200 - Full 41 Dynamic Parameters)
// Families selected by calibration-ID prefix (see config.h): M8 (67/64/32107), TC_ETC (410/325/324/32109 + default), XL (329 / 'XL'), TC_CABLE (<300)
// ----------------------------------------------------------------------------
const ECM_DID_0200_PARAMETERS = [
  { key: 'battery_voltage',               label: 'Battery Voltage',                   unit: 'V',     decimals: 2 },
  { key: 'map_kpa',                       label: 'MAP Pressure',                      unit: 'kPa',   decimals: 2 },
  { key: 'map_volts',                     label: 'MAP Sensor Voltage',                unit: 'V',     decimals: 2 },
  { key: 'engine_temperature',            label: 'Engine Temperature',                unit: '°C',    decimals: 1 },
  { key: 'engine_temperature_sensor',     label: 'Engine Temp. Sensor Voltage',       unit: 'V',     decimals: 2 },
  { key: 'intake_air_temperature',        label: 'Intake Air Temperature (IAT)',      unit: '°C',    decimals: 1 },
  { key: 'intake_air_temperature_sensor', label: 'Intake Air Temp. Sensor Voltage',   unit: 'V',     decimals: 2 },
  { key: 'throttle_position',             label: 'Throttle Position',                 unit: '%',     decimals: 2 },
  { key: 'throttle_position_sensor_1',    label: 'Throttle Position Sensor 1 (TPS1)', unit: 'V',     decimals: 2 },
  { key: 'throttle_position_sensor_2',    label: 'Throttle Position Sensor 2 (TPS2)', unit: 'V',     decimals: 2 },
  { key: 'twistgrip_position',            label: 'Twistgrip Position',                unit: '%',     decimals: 2 },
  { key: 'twistgrip_sensor_1',            label: 'Twistgrip Sensor 1 (TGS1)',         unit: 'V',     decimals: 2 },
  { key: 'twistgrip_sensor_2',            label: 'Twistgrip Sensor 2 (TGS2)',         unit: 'V',     decimals: 2 },
  { key: 'idle_set_speed',                label: 'Idle Set Speed',                    unit: 'RPM',   decimals: 0 },
  { key: 'engine_speed',                  label: 'Engine Speed',                      unit: 'RPM',   decimals: 0 },
  { key: 'vehicle_speed',                 label: 'Vehicle Speed',                     unit: 'km/h',  decimals: 0 },
  { key: 'front_closed_loop_integrator',  label: 'Front Closed Loop Integrator',      unit: '%',     decimals: 2 },
  { key: 'rear_closed_loop_integrator',   label: 'Rear Closed Loop Integrator',       unit: '%',     decimals: 2 },
  { key: 'front_adaptive_fuel_value',     label: 'Front Adaptive Fuel Value (AFV)',   unit: '%',     decimals: 2 },
  { key: 'rear_adaptive_fuel_value',      label: 'Rear Adaptive Fuel Value (AFV)',    unit: '%',     decimals: 2 },
  { key: 'front_oxygen_sensor',           label: 'Front Oxygen Sensor (O2)',          unit: 'V',     decimals: 2 },
  { key: 'rear_oxygen_sensor',            label: 'Rear Oxygen Sensor (O2)',           unit: 'V',     decimals: 2 },
  { key: 'injector_time_front',           label: 'Injector Time Front',               unit: 'ms',    decimals: 2 },
  { key: 'injector_time_rear',            label: 'Injector Time Rear',                unit: 'ms',    decimals: 2 },
  { key: 'spark_advance_front',           label: 'Spark Advance Front',               unit: 'deg',   decimals: 2 },
  { key: 'spark_advance_rear',            label: 'Spark Advance Rear',                unit: 'deg',   decimals: 2 },
  { key: 'front_spark_knock_retard',      label: 'Front Spark Knock Retard',          unit: 'deg',   decimals: 1 },
  { key: 'rear_spark_knock_retard',       label: 'Rear Spark Knock Retard',           unit: 'deg',   decimals: 1 },
  { key: 'idle_air_control_steps',        label: 'Idle Air Control Steps (IAC)',      unit: 'steps', decimals: 0 },
  { key: 'run_mode_flag',                 label: 'Run Mode Flag',                     unit: '',      isDiscrete: true },
  { key: 'tssm_security_state',           label: 'TSSM Security State',               unit: '',      isDiscrete: true },
  { key: 'bank_angle_sensor_mode',        label: 'Bank Angle Sensor Mode',            unit: '',      isDiscrete: true },
  { key: 'neutral_switch_input',          label: 'Neutral Switch Input',              unit: '',      isDiscrete: true },
  { key: 'clutch_lever_switch_state',     label: 'Clutch Lever Switch State',         unit: '',      isDiscrete: true },
  { key: 'gear_position',                 label: 'Gear Position',                     unit: '',      isDiscrete: true },
  { key: 'cruise_brake_input',            label: 'Cruise Brake Input',                unit: '',      isDiscrete: true },
  { key: 'cruise_set',                    label: 'Cruise Set Switch',                 unit: '',      isDiscrete: true },
  { key: 'cruise_res',                    label: 'Cruise Resume Switch',              unit: '',      isDiscrete: true },
  { key: 'cruise_enable_switch',          label: 'Cruise Enable Switch',              unit: '',      isDiscrete: true },
  { key: 'cruise_mode',                   label: 'Cruise Mode',                       unit: '',      isDiscrete: true },
  { key: 'cruise_rolloff_input',          label: 'Cruise Roll-Off Input',             unit: '',      isDiscrete: true }
];

// ----------------------------------------------------------------------------
// Operation 4B: ABS (0x7E1) Live Sensor Inputs (DID 0x0200)
// The ABS returns one of 4 layouts (families A-D, chosen by the firmware from the ABS
// calibration ID, see config.h 5B). The firmware only sends the parameters that exist in
// the active family, so rows are built from the keys present in each frame.
// ----------------------------------------------------------------------------
const ABS_DID_0200_PARAMETERS = [
  { key: 'front_wheel_speed',               label: 'Front Wheel Speed',                 unit: 'km/h', decimals: 2 },
  { key: 'rear_wheel_speed',                label: 'Rear Wheel Speed',                  unit: 'km/h', decimals: 2 },
  { key: 'rear_right_wheel_speed',          label: 'Rear Right Wheel Speed',            unit: 'km/h', decimals: 2 },
  { key: 'vehicle_speed',                   label: 'Vehicle Speed',                     unit: 'km/h', decimals: 2 },
  { key: 'abs_ecu_module_voltage',          label: 'Battery Voltage',                   unit: 'V',    decimals: 2 },
  { key: 'front_actuator_current',          label: 'Front Actuator Power',              unit: 'A',    decimals: 2 },
  { key: 'rear_actuator_current',           label: 'Rear Actuator Power',               unit: 'A',    decimals: 2 },
  { key: 'front_actuator_voltage',          label: 'Front Actuator Volts',              unit: 'V',    decimals: 2 },
  { key: 'rear_actuator_voltage',           label: 'Rear Actuator Volts',               unit: 'V',    decimals: 2 },
  { key: 'front_master_cylinder_pressure',  label: 'Front Master Cylinder Pressure',    unit: 'kPa',  decimals: 1 },
  { key: 'rear_master_cylinder_pressure',   label: 'Rear Master Cylinder Pressure',     unit: 'kPa',  decimals: 1 },
  { key: 'front_caliper_circuit_pressure',  label: 'Front Caliper Circuit Pressure',    unit: 'kPa',  decimals: 1 },
  { key: 'rear_caliper_circuit_pressure',   label: 'Rear Caliper Circuit Pressure',     unit: 'kPa',  decimals: 1 },
  { key: 'abs_enabled',                     label: 'ABS Enabled',                       unit: '',     isDiscrete: true },
  { key: 'abs_failed',                      label: 'ABS Failed',                        unit: '',     isDiscrete: true },
  { key: 'abs_active',                      label: 'ABS Active',                        unit: '',     isDiscrete: true },
  { key: 'front_wheel_sensor_fault',        label: 'Front Wheel Speed Sensor Fault',    unit: '',     isDiscrete: true },
  { key: 'rear_wheel_sensor_fault',         label: 'Rear Wheel Speed Sensor Fault',     unit: '',     isDiscrete: true },
  { key: 'rear_right_wheel_sensor_fault',   label: 'Rear Right Wheel Speed Sensor Fault', unit: '',   isDiscrete: true },
  { key: 'front_brake_switch',              label: 'Front Brake Switch',                unit: '',     isDiscrete: true },
  { key: 'rear_brake_switch',               label: 'Rear Brake Switch',                 unit: '',     isDiscrete: true },
  { key: 'pump_motor_status',               label: 'Pump Motor Status',                 unit: '',     isDiscrete: true },
  { key: 'front_inlet_status',              label: 'Front Inlet Status',                unit: '',     isDiscrete: true },
  { key: 'front_outlet_status',             label: 'Front Outlet Status',               unit: '',     isDiscrete: true },
  { key: 'rear_inlet_status',               label: 'Rear Inlet Status',                 unit: '',     isDiscrete: true },
  { key: 'rear_outlet_status',              label: 'Rear Outlet Status',                unit: '',     isDiscrete: true },
  { key: 'rear_right_inlet_status',         label: 'Rear Right Inlet Status',           unit: '',     isDiscrete: true },
  { key: 'rear_right_outlet_status',        label: 'Rear Right Outlet Status',          unit: '',     isDiscrete: true },
  { key: 'front_actuator',                  label: 'Front Actuator',                    unit: '',     isDiscrete: true },
  { key: 'rear_actuator',                   label: 'Rear Actuator',                     unit: '',     isDiscrete: true },
  { key: 'front_release_solenoid',          label: 'Front Release Solenoid',            unit: '',     isDiscrete: true },
  { key: 'front_apply_solenoid',            label: 'Front Apply Solenoid',              unit: '',     isDiscrete: true },
  { key: 'rear_release_solenoid',           label: 'Rear Release Solenoid',             unit: '',     isDiscrete: true },
  { key: 'rear_apply_solenoid',             label: 'Rear Apply Solenoid',               unit: '',     isDiscrete: true }
];

// Parameters actually present in a frame (keeps the table order). Before the first frame: empty.
function getAbsParamsForData(dataObj) {
  if (!dataObj || typeof dataObj !== 'object') return [];
  return ABS_DID_0200_PARAMETERS.filter(p => dataObj[p.key] !== undefined && dataObj[p.key] !== null);
}

// ----------------------------------------------------------------------------
// Operation 4C: BCM (0x7E2) Live Body & Security Inputs (DID 0x0200)
// The BCM returns one of 5 layouts (families A-E, chosen by the firmware from the BCM
// calibration ID, see config.h 5C). Only the parameters that exist in the active family
// are sent, so rows are built from the keys present in each frame.
// ----------------------------------------------------------------------------
const BCM_DID_0200_PARAMETERS = [
  { key: 'system_voltage', label: 'Battery Voltage (BCM)', unit: 'V', decimals: 2 },
  { key: 'oil_pressure_input_lamp', label: "Oil Pressure Input Lamp", unit: '', isDiscrete: true },
  { key: 'rear_brake_switch', label: "Rear Brake Switch", unit: '', isDiscrete: true },
  { key: 'neutral_switch_input', label: "Neutral Switch Input", unit: '', isDiscrete: true },
  { key: 'hi_beam_input', label: "Hi Beam Input", unit: '', isDiscrete: true },
  { key: 'horn_input', label: "Horn Input", unit: '', isDiscrete: true },
  { key: 'left_turn_switch_input', label: "Left Turn Switch Input", unit: '', isDiscrete: true },
  { key: 'hazard_switch_input', label: "Hazard Switch Input", unit: '', isDiscrete: true },
  { key: 'clutch_switch_input', label: "Clutch Switch Input", unit: '', isDiscrete: true },
  { key: 'trip_switch_status', label: "Trip Switch Status", unit: '', isDiscrete: true },
  { key: 'run_mode_flag', label: "Run Mode Flag", unit: '', isDiscrete: true },
  { key: 'start_input', label: "Start Input", unit: '', isDiscrete: true },
  { key: 'right_turn_switch_input', label: "Right Turn Switch Input", unit: '', isDiscrete: true },
  { key: 'front_brake_switch', label: "Front Brake Switch", unit: '', isDiscrete: true },
  { key: 'switched_power', label: "Switched Power", unit: '', isDiscrete: true },
  { key: 'accessory_power', label: "Accessory Power", unit: '', isDiscrete: true },
  { key: 'fuel_pump', label: "Fuel Pump", unit: '', isDiscrete: true },
  { key: 'low_beam', label: "Low Beam", unit: '', isDiscrete: true },
  { key: 'high_beam', label: "High Beam", unit: '', isDiscrete: true },
  { key: 'horn', label: "Horn", unit: '', isDiscrete: true },
  { key: 'rear_right_turn', label: "Rear Right Turn", unit: '', isDiscrete: true },
  { key: 'rear_left_turn', label: "Rear Left Turn", unit: '', isDiscrete: true },
  { key: 'brake_lamp', label: "Brake Lamp", unit: '', isDiscrete: true },
  { key: 'run_position_lights', label: "Run Position Lights", unit: '', isDiscrete: true },
  { key: 'front_right_turn', label: "Front Right Turn", unit: '', isDiscrete: true },
  { key: 'system_power', label: "System Power", unit: '', isDiscrete: true },
  { key: 'front_left_turn', label: "Front Left Turn", unit: '', isDiscrete: true },
  { key: 'tip_over_state', label: "Tip-over State", unit: '', isDiscrete: true },
  { key: 'sidecar_triglide_mode_flag', label: "Sidecar / TriGlide Mode Flag", unit: '', isDiscrete: true },
  { key: 'starter_relay_enable_output', label: "Starter Relay Enable Output", unit: '', isDiscrete: true },
  { key: 'accel_axis', label: "Accel Axis", unit: '', isDiscrete: true },
  { key: 'low_beam_switch', label: "Low Beam Switch", unit: '', isDiscrete: true },
  { key: 'aux_police_front_lts', label: "Aux/Police Front Lts", unit: '', isDiscrete: true },
  { key: 'off_switch_input', label: "Off Switch Input", unit: '', isDiscrete: true },
  { key: 'coolant_pump', label: "Coolant Pump", unit: '', isDiscrete: true }
];
const BCM_STATE_KEYS = ['oil_pressure_input_lamp', 'rear_brake_switch', 'neutral_switch_input', 'hi_beam_input', 'horn_input', 'left_turn_switch_input', 'hazard_switch_input', 'clutch_switch_input', 'trip_switch_status', 'run_mode_flag', 'start_input', 'right_turn_switch_input', 'front_brake_switch', 'switched_power', 'accessory_power', 'fuel_pump', 'low_beam', 'high_beam', 'horn', 'rear_right_turn', 'rear_left_turn', 'brake_lamp', 'run_position_lights', 'front_right_turn', 'system_power', 'front_left_turn', 'tip_over_state', 'sidecar_triglide_mode_flag', 'starter_relay_enable_output', 'accel_axis', 'low_beam_switch', 'aux_police_front_lts', 'off_switch_input', 'coolant_pump'];

// Parameters actually present in a frame (keeps the table order). Before the first frame: empty.
function getBcmParamsForData(dataObj) {
  if (!dataObj || typeof dataObj !== 'object') return [];
  return BCM_DID_0200_PARAMETERS.filter(param => dataObj[param.key] !== undefined && dataObj[param.key] !== null);
}

function isEcmActiveModule() {
  if (!currentActiveModule) return true;
  const moduleHex = String(currentActiveModule.idHex || '').toUpperCase();
  const moduleAcronym = String(currentActiveModule.acronym || '').toUpperCase();
  return moduleHex === '0X7E0' || moduleAcronym === 'ECM';
}

function isAbsActiveModule() {
  if (!currentActiveModule) return false;
  const moduleHex = String(currentActiveModule.idHex || '').toUpperCase();
  const moduleAcronym = String(currentActiveModule.acronym || '').toUpperCase();
  return moduleHex === '0X7E1' || moduleAcronym === 'ABS';
}

function isBcmActiveModule() {
  if (!currentActiveModule) return false;
  const moduleHex = String(currentActiveModule.idHex || '').toUpperCase();
  const moduleAcronym = String(currentActiveModule.acronym || '').toUpperCase();
  return moduleHex === '0X7E2' || moduleAcronym === 'BCM';
}

function isSpdoActiveModule() {
  if (!currentActiveModule) return false;
  const moduleHex = String(currentActiveModule.idHex || '').toUpperCase();
  const moduleAcronym = String(currentActiveModule.acronym || '').toUpperCase();
  return moduleHex === '0X7F2' || moduleAcronym === 'SPDO';
}

function isRadActiveModule() {
  if (!currentActiveModule) return false;
  const moduleHex = String(currentActiveModule.idHex || '').toUpperCase();
  const moduleAcronym = String(currentActiveModule.acronym || '').toUpperCase();
  return moduleHex === '0X7F1' || moduleAcronym === 'RAD';
}

function isLhcActiveModule() {
  if (!currentActiveModule) return false;
  const moduleHex = String(currentActiveModule.idHex || '').toUpperCase();
  const moduleAcronym = String(currentActiveModule.acronym || '').toUpperCase();
  return moduleHex === '0X7E3' || moduleAcronym === 'LHC';
}

const GEN_PARAM_META = {"battery_voltage":{"label":"Battery Voltage","unit":"Volts","t":"v","dec":2},"security_state":{"label":"Security State","unit":"","t":"s","on":"Disarmed","off":"Armed"},"siren":{"label":"Siren","unit":"","t":"s","on":"on","off":"off"},"horn_switch":{"label":"Horn Switch","unit":"","t":"s","on":"on","off":"off"},"high_beam_switch":{"label":"High Beam Switch","unit":"","t":"s","on":"on","off":"off"},"clutch_switch":{"label":"Clutch Switch","unit":"","t":"s","on":"Pulled In","off":"Released"},"hazard_switch":{"label":"Hazard Switch","unit":"","t":"s","on":"on","off":"off"},"start_switch":{"label":"Start Switch","unit":"","t":"s","on":"on","off":"off"},"right_turn_switch":{"label":"Right Turn Switch","unit":"","t":"s","on":"on","off":"off"},"front_brake_switch":{"label":"Front Brake Switch","unit":"","t":"s","on":"on","off":"off"},"engine_off_switch":{"label":"Engine OFF Switch","unit":"","t":"s","on":"on","off":"off"},"engine_run_switch":{"label":"Engine RUN Switch","unit":"","t":"s","on":"on","off":"off"},"front_wheel_speed":{"label":"Front Wheel Speed","unit":"Km/h","t":"v","dec":0},"rear_wheel_speed":{"label":"Rear Wheel Speed","unit":"Km/h","t":"v","dec":0},"rear_wheel_speed_sensor_fault":{"label":"Rear Wheel Speed Sensor Fault","unit":"","t":"s","on":"Yes","off":"No"},"front_wheel_speed_sensor_fault":{"label":"Front Wheel Speed Sensor Fault","unit":"","t":"s","on":"Yes","off":"No"},"rear_brake_switch":{"label":"Rear Brake Switch","unit":"","t":"s","on":"on","off":"off"},"pump_motor_status":{"label":"Pump Motor Status","unit":"","t":"s","on":"on","off":"off"},"front_inlet_status":{"label":"Front Inlet Status","unit":"","t":"s","on":"on","off":"off"},"front_outlet_status":{"label":"Front Outlet Status","unit":"","t":"s","on":"on","off":"off"},"rear_inlet_status":{"label":"Rear Inlet Status","unit":"","t":"s","on":"on","off":"off"},"rear_outlet_status":{"label":"Rear Outlet Status","unit":"","t":"s","on":"on","off":"off"},"abs_active":{"label":"ABS Active","unit":"","t":"s","on":"Yes","off":"No"},"front_caliper_circuit_pressure":{"label":"Front Caliper Circuit Pressure","unit":"kPa","t":"v","dec":1},"front_master_cylinder_pressure":{"label":"Front Master Cylinder Pressure","unit":"kPa","t":"v","dec":1},"rear_master_cylinder_pressure":{"label":"Rear Master Cylinder Pressure","unit":"kPa","t":"v","dec":1},"rear_caliper_circuit_pressure":{"label":"Rear Caliper Circuit Pressure","unit":"kPa","t":"v","dec":1},"battery_voltage_speedometer":{"label":"Battery Voltage (Speedometer)","unit":"Volts","t":"v","dec":3},"odometer_km":{"label":"Odometer(km)","unit":"","t":"v","dec":3},"fuel_sender_volts":{"label":"Fuel Sender Volts","unit":"Volts","t":"v","dec":3},"ambient_light_sensor":{"label":"Ambient Light Sensor","unit":"Volts","t":"v","dec":2},"fuel_level_percent":{"label":"Fuel Level Percent","unit":"%","t":"v","dec":1},"trip_a_displayed":{"label":"Trip A Displayed","unit":"KM","t":"v","dec":3},"trip_b_displayed":{"label":"Trip B Displayed","unit":"KM","t":"v","dec":3},"check_engine_lamp":{"label":"Check Engine Lamp","unit":"","t":"s","on":"on","off":"off"},"battery_charge_telltale":{"label":"Battery Charge Telltale","unit":"","t":"s","on":"on","off":"off"},"6th_gear_lamp":{"label":"6th Gear Lamp","unit":"","t":"s","on":"on","off":"off"},"low_fuel_telltale":{"label":"Low Fuel Telltale","unit":"","t":"s","on":"on","off":"off"},"high_beam_indicator":{"label":"High Beam Indicator","unit":"","t":"s","on":"on","off":"off"},"oil_telltale":{"label":"Oil Telltale","unit":"","t":"s","on":"on","off":"off"},"neutral_indicator_lamp":{"label":"Neutral Indicator Lamp","unit":"","t":"s","on":"on","off":"off"},"right_turn_indicator":{"label":"Right Turn Indicator","unit":"","t":"s","on":"on","off":"off"},"left_turn_indicator":{"label":"Left Turn Indicator","unit":"","t":"s","on":"on","off":"off"},"trip_keyon_displayed":{"label":"Trip KeyOn Displayed","unit":"KM","t":"v","dec":3},"left_turn_switch_input":{"label":"Left Turn Switch Input","unit":"","t":"s","on":"on","off":"off"},"traction_control_telltale":{"label":"Traction Control Telltale","unit":"","t":"s","on":"on","off":"off"},"powertrain_warning":{"label":"Powertrain Warning","unit":"","t":"s","on":"on","off":"off"},"powertrain_fault":{"label":"Powertrain Fault","unit":"","t":"s","on":"on","off":"off"},"tpms_telltale":{"label":"TPMS Telltale","unit":"","t":"s","on":"on","off":"off"},"12v_battery_telltale":{"label":"12V Battery Telltale","unit":"","t":"s","on":"on","off":"off"},"service_telltale":{"label":"Service Telltale","unit":"","t":"s","on":"on","off":"off"},"check_engine_regulatory_mil_telltale":{"label":"Check Engine / Regulatory MIL Telltale","unit":"","t":"s","on":"on","off":"off"},"fil_telltale":{"label":"FIL Telltale","unit":"","t":"s","on":"on","off":"off"},"coolant_temp_telltale":{"label":"Coolant Temp Telltale","unit":"","t":"s","on":"on","off":"off"},"abs_telltale":{"label":"ABS Telltale","unit":"","t":"s","on":"on","off":"off"},"traction_control_fault_telltale":{"label":"Traction Control Fault Telltale","unit":"","t":"s","on":"on","off":"off"},"left_turn_telltale":{"label":"Left Turn Telltale","unit":"","t":"s","on":"on","off":"off"},"right_turn_telltale":{"label":"Right Turn Telltale","unit":"","t":"s","on":"on","off":"off"},"low_tire_pressure_telltale":{"label":"Low Tire Pressure Telltale","unit":"","t":"s","on":"on","off":"off"},"lcd_intensity_level":{"label":"LCD Intensity Level","unit":"%","t":"v","dec":1},"remaining_miles_to_marry":{"label":"Remaining Miles to Marry","unit":"","t":"v","dec":0},"telltale_intensity_level":{"label":"Telltale Intensity Level","unit":"%","t":"v","dec":1},"low_fuel_trip_meter":{"label":"Low Fuel Trip Meter","unit":"KM","t":"v","dec":3},"bluetooth_status_telltale":{"label":"Bluetooth Status Telltale","unit":"","t":"s","on":"on","off":"off"},"coolant_telltale":{"label":"Coolant Telltale","unit":"","t":"s","on":"on","off":"off"},"high_beam_telltale":{"label":"High Beam Telltale","unit":"","t":"s","on":"on","off":"off"},"neutral_telltale":{"label":"Neutral Telltale","unit":"","t":"s","on":"on","off":"off"},"oil_pressure_telltale":{"label":"Oil Pressure Telltale","unit":"","t":"s","on":"on","off":"off"},"cruise_enabled_telltale_amber":{"label":"Cruise Enabled Telltale (Amber)","unit":"","t":"s","on":"on","off":"off"},"cruise_enabled_telltale_green":{"label":"Cruise Enabled Telltale (Green)","unit":"","t":"s","on":"on","off":"off"},"rear_abs_disabled_telltale":{"label":"Rear ABS Disabled Telltale","unit":"","t":"s","on":"on","off":"off"},"park_brake_telltale":{"label":"Park Brake Telltale","unit":"","t":"s","on":"on","off":"off"},"heated_grip_telltale":{"label":"Heated Grip Telltale","unit":"","t":"s","on":"on","off":"off"},"shift_detection_telltale":{"label":"Shift Detection Telltale","unit":"","t":"s","on":"on","off":"off"},"vehicle_hold_control_telltale":{"label":"Vehicle Hold Control Telltale","unit":"","t":"s","on":"on","off":"off"},"variable_ride_height_telltale":{"label":"Variable Ride Height Telltale","unit":"","t":"s","on":"on","off":"off"},"fog_lamps_aux_light":{"label":"Fog Lamps/Aux Light","unit":"","t":"s","on":"on","off":"off"},"ride_mode_telltale":{"label":"Ride Mode Telltale","unit":"","t":"s","on":"on","off":"off"},"road_ride_mode_telltale":{"label":"Road Ride Mode Telltale","unit":"","t":"s","on":"on","off":"off"},"rain_ride_mode_telltale":{"label":"Rain Ride Mode Telltale","unit":"","t":"s","on":"on","off":"off"},"sport_ride_mode_telltale":{"label":"Sport Ride Mode Telltale","unit":"","t":"s","on":"on","off":"off"}};

// ----------------------------------------------------------------------------
// Generic DID 0x0200 modules: SCM, LHC, RHC, EHCU, SPEEDO (layouts chosen by the firmware from the
// module calibration ID, see config.h 5D). Firmware sends numbers (flags as 0/1); text comes from GEN_PARAM_META.
// ----------------------------------------------------------------------------
const GEN_MODULE_IDS = ['0X7D0', '0X7E3', '0X7E5', '0X7E7', '0X7F2'];
let genStreamFrameCount = 0;
let latestGen0200Data = null;
let genPillCtx = false;

function isGenActiveModule() {
  return !!currentActiveModule && GEN_MODULE_IDS.indexOf(String(currentActiveModule.idHex || '').toUpperCase()) !== -1;
}

function activeModuleHex() {
  return currentActiveModule ? String(currentActiveModule.idHex || '') : '';
}

function isInputsKey(inputsKey) {
  return inputsKey === 'ecm' || inputsKey === 'abs' || inputsKey === 'bcm' || inputsKey === 'gen';
}

function pickByModule(modKey, absId, bcmId, ecmId) {
  if (modKey === 'abs') return absId;
  if (modKey === 'bcm') return bcmId;
  if (modKey === 'gen') return bcmId.replace('bcm', 'gen');
  return ecmId;
}

function getGenParamsForData(dataObj) {
  const paramList = [];
  if (!dataObj) return paramList;
  Object.keys(dataObj).forEach(paramKey => {
    const metaEntry = GEN_PARAM_META[paramKey];
    paramList.push(metaEntry
      ? { key: paramKey, label: metaEntry.label, unit: metaEntry.unit || '', decimals: metaEntry.dec, isDiscrete: metaEntry.t === 's' }
      : { key: paramKey, label: paramKey, unit: '' });
  });
  return paramList;
}

function genValueHtml(param, rawValue) {
  const metaEntry = GEN_PARAM_META[param.key];
  const shown = (rawValue === undefined || rawValue === null) ? undefined : ((metaEntry && metaEntry.t === 's') ? (rawValue ? metaEntry.on : metaEntry.off) : rawValue);
  genPillCtx = true;
  try { return buildEcmParameterValueHtml(param, shown); } finally { genPillCtx = false; }
}

function genPillClass(key, stateText) {
  const active = (stateText === 'ON' || stateText === 'YES' || stateText === 'PULLED IN' || stateText === 'ARMED');
  if (/fault|warning/.test(key)) return active ? 'state-red' : 'state-green';
  return active ? 'state-orange' : 'state-green';
}

function buildGenRowsHtml(params, dataObj) {
  let html = '';
  params.forEach(param => {
    html += `<div class="ecm-input-row" id="gen-row-${param.key}"><span class="ecm-input-label">${param.label}</span>` +
      `<div class="ecm-input-val-wrap" id="gen-val-${param.key}">${genValueHtml(param, dataObj ? dataObj[param.key] : undefined)}</div></div>`;
  });
  return html;
}

function updateGenInputs0200Dom(dataObj, layout) {
  inputsFrameArrived();
  if (!dataObj || typeof dataObj !== 'object') return;
  latestGen0200Data = dataObj;
  genStreamFrameCount++;

  let gridEl = document.getElementById('gen-inputs-grid');
  if (!gridEl && currentActiveOperation === 'inputs' && isGenActiveModule()) {
    renderModuleInputsView();
    gridEl = document.getElementById('gen-inputs-grid');
  }
  if (!gridEl) return;

  const params = getGenParamsForData(dataObj);
  const paramKeysSignature = params.map(param => param.key).join(',');
  if (gridEl.getAttribute('data-keys') !== paramKeysSignature) {
    gridEl.setAttribute('data-keys', paramKeysSignature);
    gridEl.innerHTML = buildGenRowsHtml(params, dataObj);
  } else {
    params.forEach(param => {
      const valueElement = document.getElementById(`gen-val-${param.key}`);
      if (valueElement) valueElement.innerHTML = genValueHtml(param, dataObj[param.key]);
    });
  }
  const famEl = document.getElementById('gen-family-label');
  if (famEl && layout) famEl.textContent = `Layout ${layout}`;
  const badgeEl = document.getElementById('gen-stream-status-badge');
  if (badgeEl) {
    badgeEl.classList.toggle('paused', !ecmStreamActive);
    const txtEl = document.getElementById('gen-stream-status-text');
    if (txtEl) txtEl.textContent = ecmStreamActive ? `LIVE (#${genStreamFrameCount})` : 'PAUSED';
  }
}


function getActiveInputsModuleKey() {
  if (isAbsActiveModule()) return 'abs';
  if (isBcmActiveModule()) return 'bcm';
  if (isEcmActiveModule()) return 'ecm';
  if (isGenActiveModule()) return 'gen';
  return String((currentActiveModule && currentActiveModule.acronym) || 'other').toLowerCase();
}

const ABS_FAULT_KEYS = ['abs_failed', 'front_wheel_sensor_fault', 'rear_wheel_sensor_fault', 'rear_right_wheel_sensor_fault'];
const ABS_ONOFF_KEYS = ['front_brake_switch', 'rear_brake_switch', 'pump_motor_status', 'front_inlet_status', 'front_outlet_status',
  'rear_inlet_status', 'rear_outlet_status', 'rear_right_inlet_status', 'rear_right_outlet_status', 'front_actuator', 'rear_actuator',
  'front_release_solenoid', 'front_apply_solenoid', 'rear_release_solenoid', 'rear_apply_solenoid'];

function formatDiscretePillClass(key, valStr) {
  const stateString = String(valStr || '').toUpperCase();
  if (stateString === 'N/A') return 'state-orange';
  if (genPillCtx) return genPillClass(key, stateString);
  // ABS (0x7E1) Yes/No flags and On/Off switches, solenoids & pump
  if (ABS_FAULT_KEYS.indexOf(key) !== -1) return stateString === 'YES' ? 'state-red' : 'state-green';
  if (key === 'abs_active') return stateString === 'YES' ? 'state-orange' : 'state-green';
  if (key === 'abs_enabled') return stateString === 'YES' ? 'state-green' : 'state-orange';
  if (ABS_ONOFF_KEYS.indexOf(key) !== -1) return stateString === 'ON' ? 'state-orange' : 'state-green';
  // BCM (0x7E2) switches, lamps & outputs (neutral / run-mode keep their shared rules below)
  if (key === 'tip_over_state') return stateString === 'TIPPED' ? 'state-red' : 'state-green';
  if (key === 'oil_pressure_input_lamp') return stateString === 'ON' ? 'state-red' : 'state-green';
  if (BCM_STATE_KEYS.indexOf(key) !== -1 && key !== 'neutral_switch_input' && key !== 'run_mode_flag') {
    return (stateString === 'ON' || stateString === 'PRESSED' || stateString === 'PULLED IN' || stateString === 'ACTIVE' || stateString === 'YES' || stateString === 'Z AXIS') ? 'state-orange' : 'state-green';
  }
  if (key === 'fuel_mixture_state') {
    return stateString.indexOf('RICH') !== -1 ? 'state-orange' : 'state-green';
  }
  if (key === 'engine_run_stop_switch' || key === 'run_mode_flag' || key === 'kill_switch_state') {
    return (stateString === 'RUN' || stateString === 'ON') ? 'state-green' : 'state-red';
  }
  if (key === 'vehicle_theft_deterrent' || key === 'tssm_security_state' || key === 'security_fob_status') {
    return stateString === 'ARMED' ? 'state-orange' : 'state-green';
  }
  if (key === 'vehicle_tipped' || key === 'bank_angle_sensor_mode') {
    return stateString === 'TIPPED' ? 'state-red' : 'state-green';
  }
  if (key === 'neutral_switch' || key === 'neutral_switch_input' || key === 'gear_position') {
    return (stateString === 'IN NEUTRAL' || stateString === 'NEUTRAL') ? 'state-green' : 'state-orange';
  }
  if (key === 'clutch_switch' || key === 'clutch_lever_switch_state' || key === 'start_button_state' || key === 'horn_switch_state') {
    return stateString === 'PRESSED' ? 'state-orange' : 'state-green';
  }
  if (key === 'front_brake_switch' || key === 'rear_brake_switch' || key === 'left_turn_switch' || key === 'right_turn_switch') {
    return (stateString === 'APPLIED' || stateString === 'ACTIVE') ? 'state-orange' : 'state-green';
  }
  if (key === 'high_beam_state') {
    return stateString.indexOf('HIGH') !== -1 ? 'state-orange' : 'state-green';
  }
  if (key.indexOf('cruise_') === 0) {
    return stateString === 'ON' ? 'state-orange' : 'state-green';
  }
  // ABS (0x7E1) Discrete Switches, Lamp, Pump Relay & Hydraulic Solenoid Valves
  if (key === 'front_brake_light_switch' || key === 'rear_brake_light_switch') {
    return stateString === 'ACTIVE' ? 'state-orange' : 'state-green';
  }
  if (key === 'abs_warning_lamp_state') {
    return stateString === 'LAMP ON' ? 'state-red' : 'state-green';
  }
  if (key === 'abs_motor_pump_relay_status') {
    return stateString === 'PUMP RUNNING' ? 'state-orange' : 'state-green';
  }
  if (
    key === 'front_inlet_valve_state' ||
    key === 'front_outlet_valve_state' ||
    key === 'rear_inlet_valve_state' ||
    key === 'rear_outlet_valve_state'
  ) {
    return stateString === 'VALVE OPEN' ? 'state-orange' : 'state-green';
  }
  return 'state-green';
}

function buildEcmParameterValueHtml(param, rawVal) {
  if (rawVal === undefined || rawVal === null || rawVal === '--') {
    return `<span class="ecm-input-value val-orange">--</span>${param.unit ? `<span class="ecm-input-unit">${param.unit}</span>` : ''}`;
  }
  if (rawVal === 'N/A') {
    return `<span class="ecm-state-pill state-orange">N/A</span>`;
  }
  if (param.isDiscrete || typeof rawVal === 'string') {
    const strVal = String(rawVal);
    const pillClass = formatDiscretePillClass(param.key, strVal);
    return `<span class="ecm-state-pill ${pillClass}">${strVal}</span>`;
  }
  const numericValue = Number(rawVal);
  const formatted = isNaN(numericValue) ? String(rawVal) : numericValue.toFixed(param.decimals !== undefined ? param.decimals : 1);
  return `<span class="ecm-input-value">${formatted}</span>${param.unit ? `<span class="ecm-input-unit">${param.unit}</span>` : ''}`;
}

function updateAbsInputs0200Dom(dataObj) {
  inputsFrameArrived();
  if (!dataObj || typeof dataObj !== 'object') return;
  latestAbs0200Data = dataObj;
  absStreamFrameCount++;

  let gridEl = document.getElementById('abs-inputs-grid');
  if (!gridEl && currentActiveOperation === 'inputs' && isAbsActiveModule()) {
    renderModuleInputsView();
    gridEl = document.getElementById('abs-inputs-grid');
  }
  if (!gridEl) return;

  const absParams = getAbsParamsForData(dataObj);
  const paramKeysSignature = absParams.map(p => p.key).join(',');
  if (gridEl.getAttribute('data-keys') !== paramKeysSignature) {
    gridEl.innerHTML = '';
    gridEl.setAttribute('data-keys', paramKeysSignature);
    absParams.forEach(param => {
      const rowElement = document.createElement('div');
      rowElement.className = 'ecm-input-row';
      rowElement.id = `abs-row-${param.key}`;
      rowElement.innerHTML = `
        <span class="ecm-input-label">${param.label}</span>
        <div class="ecm-input-val-wrap" id="abs-val-${param.key}">
          ${buildEcmParameterValueHtml(param, dataObj[param.key])}
        </div>
      `;
      gridEl.appendChild(rowElement);
    });
  } else {
    absParams.forEach(param => {
      const valWrap = document.getElementById(`abs-val-${param.key}`);
      if (valWrap) {
        valWrap.innerHTML = buildEcmParameterValueHtml(param, dataObj[param.key]);
      }
    });
  }

  const famEl = document.getElementById('abs-family-label');
  if (famEl && dataObj.abs_family) famEl.textContent = `Layout ${dataObj.abs_family}`;

  const badgeEl = document.getElementById('abs-stream-status-badge');
  if (badgeEl) {
    badgeEl.classList.toggle('paused', !ecmStreamActive);
    const txtEl = document.getElementById('abs-stream-status-text');
    if (txtEl) {
      txtEl.textContent = ecmStreamActive ? `LIVE (#${absStreamFrameCount})` : 'PAUSED';
    }
  }

  // Keep ABS Actuator Wheel Speed Safety Lock synchronized whenever ABS DID 0x0200 updates
  if (typeof updateAbsOutputsHeaderTelemetry === 'function') {
    updateAbsOutputsHeaderTelemetry(
      dataObj.abs_ecu_module_voltage,
      dataObj.front_wheel_speed !== undefined ? dataObj.front_wheel_speed : dataObj.vehicle_speed,
      Math.max(Number(dataObj.rear_wheel_speed || 0), Number(dataObj.rear_right_wheel_speed || 0), Number(dataObj.vehicle_speed || 0))
    );
  }
}

function updateBcmInputs0200Dom(dataObj) {
  inputsFrameArrived();
  if (!dataObj || typeof dataObj !== 'object') return;
  latestBcm0200Data = dataObj;
  bcmStreamFrameCount++;

  let gridEl = document.getElementById('bcm-inputs-grid');
  if (!gridEl && currentActiveOperation === 'inputs' && isBcmActiveModule()) {
    renderModuleInputsView();
    gridEl = document.getElementById('bcm-inputs-grid');
  }
  if (!gridEl) return;

  const bcmParams = getBcmParamsForData(dataObj);
  const paramKeysSignature = bcmParams.map(p => p.key).join(',');
  if (gridEl.getAttribute('data-keys') !== paramKeysSignature) {
    gridEl.innerHTML = '';
    gridEl.setAttribute('data-keys', paramKeysSignature);
    bcmParams.forEach(param => {
      const rowElement = document.createElement('div');
      rowElement.className = 'ecm-input-row';
      rowElement.id = `bcm-row-${param.key}`;
      rowElement.innerHTML = `
        <span class="ecm-input-label">${param.label}</span>
        <div class="ecm-input-val-wrap" id="bcm-val-${param.key}">
          ${buildEcmParameterValueHtml(param, dataObj[param.key])}
        </div>
      `;
      gridEl.appendChild(rowElement);
    });
  } else {
    bcmParams.forEach(param => {
      const valWrap = document.getElementById(`bcm-val-${param.key}`);
      if (valWrap) {
        valWrap.innerHTML = buildEcmParameterValueHtml(param, dataObj[param.key]);
      }
    });
  }

  const bcmFamEl = document.getElementById('bcm-family-label');
  if (bcmFamEl && dataObj.bcm_family) bcmFamEl.textContent = `Layout ${dataObj.bcm_family}`;

  const badgeEl = document.getElementById('bcm-stream-status-badge');
  if (badgeEl) {
    badgeEl.classList.toggle('paused', !ecmStreamActive);
    const txtEl = document.getElementById('bcm-stream-status-text');
    if (txtEl) {
      txtEl.textContent = ecmStreamActive ? `LIVE (#${bcmStreamFrameCount})` : 'PAUSED';
    }
  }
}

function updateEcmInputs0200Dom(dataObj) {
  inputsFrameArrived();
  if (!dataObj || typeof dataObj !== 'object') return;
  latestEcm0200Data = dataObj;
  ecmStreamFrameCount++;

  let gridEl = document.getElementById('ecm-inputs-grid');
  if (!gridEl && currentActiveOperation === 'inputs' && isEcmActiveModule()) {
    renderModuleInputsView();
    gridEl = document.getElementById('ecm-inputs-grid');
  }
  if (!gridEl) return;

  // Ensure all 41 parameter rows exist in the grid; create dynamically if empty
  if (gridEl.children.length !== ECM_DID_0200_PARAMETERS.length) {
    gridEl.innerHTML = '';
    ECM_DID_0200_PARAMETERS.forEach(param => {
      const rowElement = document.createElement('div');
      rowElement.className = 'ecm-input-row';
      rowElement.id = `ecm-row-${param.key}`;
      rowElement.innerHTML = `
        <span class="ecm-input-label">${param.label}</span>
        <div class="ecm-input-val-wrap" id="ecm-val-${param.key}">
          ${buildEcmParameterValueHtml(param, dataObj[param.key])}
        </div>
      `;
      gridEl.appendChild(rowElement);
    });
  } else {
    // Fast in-place update of all 41 parameter values
    ECM_DID_0200_PARAMETERS.forEach(param => {
      const valWrap = document.getElementById(`ecm-val-${param.key}`);
      if (valWrap && dataObj[param.key] !== undefined) {
        valWrap.innerHTML = buildEcmParameterValueHtml(param, dataObj[param.key]);
      }
    });
  }

  const badgeEl = document.getElementById('ecm-stream-status-badge');
  if (badgeEl) {
    badgeEl.classList.toggle('paused', !ecmStreamActive);
    const txtEl = document.getElementById('ecm-stream-status-text');
    if (txtEl) {
      txtEl.textContent = ecmStreamActive ? `LIVE (#${ecmStreamFrameCount})` : 'PAUSED';
    }
  }
}

function renderModuleInputsView() {
  const container = document.getElementById('op-content-container');
  if (!container) return;

  const modHex = currentActiveModule ? currentActiveModule.idHex : '0x7E0';
  const modAbbr = currentActiveModule ? currentActiveModule.acronym : 'ECM';
  const isAbs = isAbsActiveModule();
  const isBcm = isBcmActiveModule();
  const isEcm = isEcmActiveModule();

  if (isAbs) {
    let absRowsHtml = '';
    const absInitParams = getAbsParamsForData(latestAbs0200Data);
    absInitParams.forEach(param => {
      absRowsHtml += `
        <div class="ecm-input-row" id="abs-row-${param.key}">
          <span class="ecm-input-label">${param.label}</span>
          <div class="ecm-input-val-wrap" id="abs-val-${param.key}">
            ${buildEcmParameterValueHtml(param, latestAbs0200Data[param.key])}
          </div>
        </div>
      `;
    });
    if (!absRowsHtml) {
      absRowsHtml = '<div class="ecm-input-row"><span class="ecm-input-label">Waiting for ABS DID 0x0200 data...</span></div>';
    }
    const absInitSig = absInitParams.map(p => p.key).join(',');

    container.innerHTML = `
      <div class="results-card" id="abs-live-sensor-card" data-module="0x7E1" data-did="0200">
        <div class="results-header-row ecm-telemetry-toolbar">
          <div>
            <span class="results-title">ABS LIVE SENSOR INPUTS</span>
            <span class="ecm-input-unit" id="abs-family-label" style="margin-left:8px;"></span>
          </div>
          <div class="inputs-toolbar-controls">
            <div class="inputs-toolbar-buttons">
              <button type="button" class="btn btn-primary btn-xs btn-read-now" onclick="requestEcmInputs0200Once()">
                <span>Read Now</span>
              </button>
              <button type="button" class="btn btn-secondary btn-xs" id="btn-toggle-abs-stream" onclick="toggleEcmLiveStream0200()">
                <span id="btn-toggle-abs-stream-lbl">Pause</span>
              </button>
            </div>
            <span class="ecm-stream-badge" id="abs-stream-status-badge">
              <span class="ecm-stream-dot"></span>
              <span id="abs-stream-status-text">LIVE</span>
            </span>
          </div>
        </div>
        <div id="abs-inputs-error-banner" class="hidden" style="padding:10px 12px;background:rgba(244,63,94,0.1);border:1px solid rgba(244,63,94,0.3);border-radius:var(--radius-md);color:#fb7185;font-size:12px;font-weight:700;"></div>
        <div class="ecm-inputs-grid" id="abs-inputs-grid" data-did="0200" data-module="0x7E1" data-keys="${absInitSig}">
          ${absRowsHtml}
        </div>
      </div>
    `;

    startEcmLiveStream0200();
    return;
  }

  if (isBcm) {
    let bcmRowsHtml = '';
    const bcmInitParams = getBcmParamsForData(latestBcm0200Data);
    bcmInitParams.forEach(param => {
      bcmRowsHtml += `
        <div class="ecm-input-row" id="bcm-row-${param.key}">
          <span class="ecm-input-label">${param.label}</span>
          <div class="ecm-input-val-wrap" id="bcm-val-${param.key}">
            ${buildEcmParameterValueHtml(param, latestBcm0200Data[param.key])}
          </div>
        </div>
      `;
    });
    if (!bcmRowsHtml) {
      bcmRowsHtml = '<div class="ecm-input-row"><span class="ecm-input-label">Waiting for BCM DID 0x0200 data...</span></div>';
    }
    const bcmInitSig = bcmInitParams.map(p => p.key).join(',');

    container.innerHTML = `
      <div class="results-card" id="bcm-live-sensor-card" data-module="0x7E2" data-did="0200">
        <div class="results-header-row ecm-telemetry-toolbar">
          <div>
            <span class="results-title">BCM LIVE BODY &amp; SECURITY INPUTS</span>
            <span class="ecm-input-unit" id="bcm-family-label" style="margin-left:8px;"></span>
          </div>
          <div class="inputs-toolbar-controls">
            <div class="inputs-toolbar-buttons">
              <button type="button" class="btn btn-primary btn-xs btn-read-now" onclick="requestEcmInputs0200Once()">
                <span>Read Now</span>
              </button>
              <button type="button" class="btn btn-secondary btn-xs" id="btn-toggle-bcm-stream" onclick="toggleEcmLiveStream0200()">
                <span id="btn-toggle-bcm-stream-lbl">Pause</span>
              </button>
            </div>
            <span class="ecm-stream-badge" id="bcm-stream-status-badge">
              <span class="ecm-stream-dot"></span>
              <span id="bcm-stream-status-text">LIVE</span>
            </span>
          </div>
        </div>
        <div id="bcm-inputs-error-banner" class="hidden" style="padding:10px 12px;background:rgba(244,63,94,0.1);border:1px solid rgba(244,63,94,0.3);border-radius:var(--radius-md);color:#fb7185;font-size:12px;font-weight:700;"></div>
        <div class="ecm-inputs-grid" id="bcm-inputs-grid" data-did="0200" data-module="0x7E2" data-keys="${bcmInitSig}">
          ${bcmRowsHtml}
        </div>
      </div>
    `;

    startEcmLiveStream0200();
    return;
  }

  if (isGenActiveModule()) {
    const genParams = getGenParamsForData(latestGen0200Data);
    const genRows = genParams.length ? buildGenRowsHtml(genParams, latestGen0200Data)
      : `<div class="ecm-input-row"><span class="ecm-input-label">Waiting for ${modAbbr || modHex} DID 0x0200 data...</span></div>`;
    container.innerHTML = `
      <div class="results-card" id="gen-live-sensor-card" data-module="${modHex}" data-did="0200">
        <div class="results-header-row ecm-telemetry-toolbar">
          <div>
            <span class="results-title">${modAbbr} LIVE INPUTS</span>
            <span class="ecm-input-unit" id="gen-family-label" style="margin-left:8px;"></span>
          </div>
          <div class="inputs-toolbar-controls">
            <div class="inputs-toolbar-buttons">
              <button type="button" class="btn btn-primary btn-xs btn-read-now" onclick="requestEcmInputs0200Once()">
                <span>Read Now</span>
              </button>
              <button type="button" class="btn btn-secondary btn-xs" id="btn-toggle-gen-stream" onclick="toggleEcmLiveStream0200()">
                <span id="btn-toggle-gen-stream-lbl">Pause</span>
              </button>
            </div>
            <span class="ecm-stream-badge" id="gen-stream-status-badge">
              <span class="ecm-stream-dot"></span>
              <span id="gen-stream-status-text">LIVE</span>
            </span>
          </div>
        </div>
        <div id="gen-inputs-error-banner" class="hidden" style="padding:10px 12px;background:rgba(244,63,94,0.1);border:1px solid rgba(244,63,94,0.3);border-radius:var(--radius-md);color:#fb7185;font-size:12px;font-weight:700;"></div>
        <div class="ecm-inputs-grid" id="gen-inputs-grid" data-did="0200" data-module="${modHex}" data-keys="${genParams.map(p => p.key).join(',')}">
          ${genRows}
        </div>
      </div>
    `;
    startEcmLiveStream0200();
    return;
  }

  if (!isEcm) {
    stopEcmLiveStream0200();
    container.innerHTML = `
      <div class="results-card" data-module="${modHex}">
        <div class="results-header-row">
          <span class="results-title">${modAbbr || modHex} Module Inputs</span>
        </div>
        <div style="padding:16px;background:rgba(245,158,11,0.08);border:1px solid rgba(245,158,11,0.28);border-radius:var(--radius-md);color:#fcd34d;font-size:12.5px;">
          Live DID 0x0200 Input Streaming is not available for <strong>${modAbbr || modHex} (${modHex})</strong>: this module has no data-stream definition.
        </div>
      </div>
    `;
    return;
  }

  let rowsHtml = '';
  ECM_DID_0200_PARAMETERS.forEach(param => {
    const initialVal = latestEcm0200Data ? latestEcm0200Data[param.key] : '--';
    rowsHtml += `
      <div class="ecm-input-row" id="ecm-row-${param.key}">
        <span class="ecm-input-label">${param.label}</span>
        <div class="ecm-input-val-wrap" id="ecm-val-${param.key}">
          ${buildEcmParameterValueHtml(param, initialVal)}
        </div>
      </div>
    `;
  });

  container.innerHTML = `
    <div class="results-card" data-module="0x7E0" data-did="0200">
      <div class="results-header-row ecm-telemetry-toolbar">
        <div>
          <span class="results-title">ECM Live Module Inputs (41 Parameters)</span>
        </div>
        <div class="inputs-toolbar-controls">
          <div class="inputs-toolbar-buttons">
            <button type="button" class="btn btn-primary btn-xs btn-read-now" onclick="requestEcmInputs0200Once()">
              <span>Read Now</span>
            </button>
            <button type="button" class="btn btn-secondary btn-xs" id="btn-toggle-ecm-stream" onclick="toggleEcmLiveStream0200()">
              <span id="btn-toggle-ecm-stream-lbl">Pause</span>
            </button>
          </div>
          <span class="ecm-stream-badge" id="ecm-stream-status-badge">
            <span class="ecm-stream-dot"></span>
            <span id="ecm-stream-status-text">LIVE</span>
          </span>
        </div>
      </div>
      <div id="ecm-inputs-error-banner" class="hidden" style="padding:10px 12px;background:rgba(244,63,94,0.1);border:1px solid rgba(244,63,94,0.3);border-radius:var(--radius-md);color:#fb7185;font-size:12px;font-weight:700;"></div>
      <div class="ecm-inputs-grid" id="ecm-inputs-grid" data-did="0200" data-module="${modHex}">
        ${rowsHtml}
      </div>
    </div>
  `;

  startEcmLiveStream0200();
}

// ----------------------------------------------------------------------------
// Read Inputs: stale-data protection + "no communication" detection
// ----------------------------------------------------------------------------
let inputsLastFrameMs = 0;
let inputsViewStartMs = 0;
let inputsNoComm = false;
let inputsGoodCount = 0;        // consecutive good reads since an error (banner clears at 8 ≈ 4 s)
let inputsWatchTimer = null;

function inputsResetCache() {
  latestEcm0200Data = null;
  latestAbs0200Data = null;
  latestBcm0200Data = null;
  latestGen0200Data = null;
  ecmStreamFrameCount = 0;
  absStreamFrameCount = 0;
  bcmStreamFrameCount = 0;
  genStreamFrameCount = 0;
  inputsLastFrameMs = 0;
  inputsNoComm = false;
}

function inputsHideAllErrorBanners() {
  ['ecm', 'abs', 'bcm', 'gen'].forEach(moduleKey => {
    const banner = document.getElementById(moduleKey + '-inputs-error-banner');
    if (banner) banner.classList.add('hidden');
  });
}
// Clear the "no communication" error immediately (used by a successful manual Read Now).
function inputsClearNoComm() {
  inputsNoComm = false;
  inputsGoodCount = 0;
  inputsHideAllErrorBanners();
}
function inputsFrameArrived() {
  inputsLastFrameMs = Date.now();
  // When an error is showing, clear it only after 8 consecutive good reads (~4 s at 500 ms), so a flaky
  // bus does not flicker the banner on and off. A manual Read Now clears it at once.
  if (inputsNoComm) {
    inputsGoodCount++;
    if (inputsGoodCount >= 8) inputsClearNoComm();
  } else {
    inputsGoodCount = 0;
  }
}

function inputsShowNoComm() {
  if (currentActiveOperation !== 'inputs' || !currentActiveModule) return;
  const modKey = getActiveInputsModuleKey();
  const abbr = currentActiveModule.acronym || currentActiveModule.idHex;
  inputsNoComm = true;
  inputsGoodCount = 0;        // restart the 8-good-reads requirement for this new error
  // forget the old values: never keep showing a previous read as if it were current
  if (modKey === 'abs') latestAbs0200Data = null;
  else if (modKey === 'bcm') latestBcm0200Data = null;
  else if (modKey === 'gen') latestGen0200Data = null;
  else if (modKey === 'ecm') latestEcm0200Data = null;

  const banner = document.getElementById(modKey + '-inputs-error-banner');
  if (banner) {
    banner.classList.remove('hidden');
    banner.textContent = `No communication with ${abbr} (${currentActiveModule.idHex}). Check that the ignition is in RUN, the CAN wiring and that the module is fitted.`;
  }
  const grid = document.getElementById(modKey + '-inputs-grid');
  if (grid) {
    grid.setAttribute('data-keys', '');
    grid.innerHTML = `<div class="ecm-input-row"><span class="ecm-input-label">Waiting for ${abbr} DID 0x0200 data...</span></div>`;
  }
  const streamStatusText = document.getElementById(modKey + '-stream-status-text');
  if (streamStatusText) streamStatusText.textContent = 'NO COMMUNICATION';
}

function inputsStartWatchdog() {
  inputsViewStartMs = Date.now();
  inputsLastFrameMs = 0;
  inputsNoComm = false;
  if (inputsWatchTimer) clearInterval(inputsWatchTimer);
  inputsWatchTimer = setInterval(() => {
    if (currentActiveOperation !== 'inputs' || !document.getElementById('op-content-container')) {
      clearInterval(inputsWatchTimer); inputsWatchTimer = null; return;
    }
    if (!ecmStreamActive) return;                       // paused by the user
    const now = Date.now();
    const lastSign = inputsLastFrameMs || inputsViewStartMs;
    if (now - lastSign > 3000 && !inputsNoComm) inputsShowNoComm();
  }, 1000);
}

// Toggle the visible "Read Now" button between its normal label and a "Reading…" busy state.
// Read Now is only usable while the live stream is PAUSED (when streaming, reads already arrive live).
function setReadNowBusy(busy) {
  const btn = document.querySelector('#op-content-container .btn-read-now');
  if (!btn) return;
  const lock = busy || !!ecmStreamActive;
  btn.disabled = lock;
  btn.style.opacity = lock ? '0.5' : '1';
  const lbl = btn.querySelector('span');
  if (lbl) lbl.textContent = busy ? 'Reading…' : 'Read Now';
}
// Refresh the Read Now enabled state when streaming starts/stops (enabled only when paused).
function updateReadNowEnabled() { setReadNowBusy(false); }

function requestEcmInputs0200Once(withStream) {
  if (currentActiveOperation !== 'inputs') return;
  const modHex = currentActiveModule ? currentActiveModule.idHex : '0x7E0';
  const modKey = getActiveInputsModuleKey();
  if (!isInputsKey(modKey)) return;
  isDiagOperationRunning = true;
  setReadNowBusy(true);

  fetch(`/api/uds/inputs?id=${encodeURIComponent(modHex)}&module=${encodeURIComponent(modKey)}&did=0x0200${withStream === true ? '&stream=1' : ''}`, { method: 'POST', cache: 'no-store' })
    .then(response => response.json())
    .then(payload => {
      isDiagOperationRunning = false;
      if (payload && (payload.type === 'live_data' || payload.ok) && String(payload.did || '').replace(/^0x/i, '') === '0200' && payload.data) {
        inputsClearNoComm();      // a successful manual read clears the error immediately
        const rxMod = String(payload.module || modKey).toLowerCase();
        if (payload.gen_layout !== undefined) {
          updateGenInputs0200Dom(payload.data, payload.gen_layout);
        } else if (rxMod === 'abs') {
          updateAbsInputs0200Dom(payload.data);
        } else if (rxMod === 'bcm') {
          updateBcmInputs0200Dom(payload.data);
        } else {
          updateEcmInputs0200Dom(payload.data);
        }
      } else if (payload && payload.no_reply) {
        inputsShowNoComm();
      }
    })
    .catch(() => {
      isDiagOperationRunning = false;
    })
    .then(() => setReadNowBusy(false));
}

function startEcmLiveStream0200() {
  const modKey = getActiveInputsModuleKey();
  if (!isInputsKey(modKey)) {
    stopEcmLiveStream0200();
    return;
  }

  ecmStreamActive = true;
  updateReadNowEnabled();     // streaming -> Read Now disabled
  inputsStartWatchdog();
  if (ecmStreamPollTimer) {
    clearInterval(ecmStreamPollTimer);
    ecmStreamPollTimer = null;
  }

  const btnLblId = pickByModule(modKey, 'btn-toggle-abs-stream-lbl', 'btn-toggle-bcm-stream-lbl', 'btn-toggle-ecm-stream-lbl');
  const badgeId = pickByModule(modKey, 'abs-stream-status-badge', 'bcm-stream-status-badge', 'ecm-stream-status-badge');

  const btnLbl = document.getElementById(btnLblId);
  if (btnLbl) btnLbl.textContent = 'Pause';
  const badgeEl = document.getElementById(badgeId);
  if (badgeEl) badgeEl.classList.remove('paused');

  // Notify ESP32 to stream DID 0x0200 for the active module (ecm, abs, or bcm).
  // ORDERING FIX: a previous stopEcmLiveStream0200() (called by navigateToOperation) may still have an
  // HTTP "stream?enable=0" request in flight. The single-threaded ESP32 server would process it AFTER
  // our WebSocket start_stream and silently cancel the stream. So wait for that request first.
  const mySeq = ++ecmStartSeq;
  Promise.resolve(ecmStopPending).then(() => {
    if (mySeq !== ecmStartSeq || !ecmStreamActive) return;   // superseded by a newer start/stop
    if (telemetryWs && telemetryWs.readyState === WebSocket.OPEN) {
      try {
        telemetryWs.send(JSON.stringify({ cmd: 'start_stream', module: modKey, id: activeModuleHex(), did: '0200' }));
      } catch (e) {}
    } else {
      initTelemetryWebSocket();   // onopen sends start_stream
    }
    // Trigger a single initial read; all subsequent updates arrive via the 200ms WebSocket push stream.
    // If the WebSocket is not open yet, also ask the HTTP endpoint to enable streaming (fallback).
    requestEcmInputs0200Once(!(telemetryWs && telemetryWs.readyState === WebSocket.OPEN));
  });
}

function stopEcmLiveStream0200() {
  ecmStreamActive = false;
  updateReadNowEnabled();     // paused -> Read Now enabled
  ecmStartSeq++;   // cancel any delayed start
  if (ecmStreamPollTimer) {
    clearInterval(ecmStreamPollTimer);
    ecmStreamPollTimer = null;
  }
  const modKey = getActiveInputsModuleKey();
  if (telemetryWs && telemetryWs.readyState === WebSocket.OPEN) {
    // WebSocket is ordered: a later start_stream can never be overtaken by this stop.
    try {
      telemetryWs.send(JSON.stringify({ cmd: 'stop_stream', module: modKey, id: activeModuleHex(), did: '0200' }));
    } catch (e) {}
    ecmStopPending = null;
  } else {
    // No WebSocket: fall back to HTTP and remember the request so a following start waits for it.
    ecmStopPending = fetch('/api/uds/inputs/stream?enable=0', { method: 'POST', cache: 'no-store' })
      .catch(() => {})
      .then(() => { ecmStopPending = null; });
  }
}

function toggleEcmLiveStream0200() {
  const modKey = getActiveInputsModuleKey();
  const btnLblId = pickByModule(modKey, 'btn-toggle-abs-stream-lbl', 'btn-toggle-bcm-stream-lbl', 'btn-toggle-ecm-stream-lbl');
  const badgeId = pickByModule(modKey, 'abs-stream-status-badge', 'bcm-stream-status-badge', 'ecm-stream-status-badge');
  const txtId = pickByModule(modKey, 'abs-stream-status-text', 'bcm-stream-status-text', 'ecm-stream-status-text');

  if (ecmStreamActive) {
    stopEcmLiveStream0200();
    const btnLbl = document.getElementById(btnLblId);
    if (btnLbl) btnLbl.textContent = 'Resume';
    const badgeEl = document.getElementById(badgeId);
    if (badgeEl) badgeEl.classList.add('paused');
    const txtEl = document.getElementById(txtId);
    if (txtEl) txtEl.textContent = 'PAUSED';
  } else {
    startEcmLiveStream0200();
  }
}

// ----------------------------------------------------------------------------
// Operation 5: Set Module Outputs (ECM 0x7E0 & ABS 0x7E1 - UDS Service 0x2F)
// Includes:
//   1. ECM (0x7E0) 7 Core Actuator Command Map (Pulse 2s, Toggle, Test Output)
//   2. ABS (0x7E1) "ABS ACTUATOR TESTS" Command Map:
//      - Extended Session Switch: 02 10 03 00 00 00 00 00 to 0x7E1
//      - Periodic TesterPresent (2000ms): 02 3E 80 00 00 00 00 00 to 0x7E1
//      - ABS Hydraulic Pump Motor (DID 0x0540) -> Button: "Test Pump (1s)"
//        * Non-blocking 1000ms pulse: 06 2F 05 40 03 20 20 00 -> 04 2F 05 40 00 00 00 00
//      - Wheel Speed Safety Lock (km/h > 0): Disables button with "Disabled: Vehicle is Moving"
//      - Negative Response (NRC) Interception (03 7F 2F [NRC], e.g. 0x22 -> "Conditions Not Correct")
//      - 3000ms UI Timeout Guard ("ABS Module Communication Timeout")
//      - Failsafe Cleanup on exit/disconnect: 04 2F 05 40 00 00 00 00 + 02 10 01 00 00 00 00 00
// ----------------------------------------------------------------------------
const ABS_OUTPUT_ACTUATORS = [
  {
    did: '0540',
    didHex: '0x0540',
    targetId: '0x7E1',
    responseId: '0x7E9',
    label: 'ABS Hydraulic Pump Motor',
    btnText: 'Test Pump (1s)',
    mode: 'pulse_1s',
    pulseMs: 1000,
    txSession: '02 10 03 00 00 00 00 00',
    txHeartbeat: '02 3E 80 00 00 00 00 00',
    txOn: '06 2F 05 40 03 20 20 00',
    txReset: '04 2F 05 40 00 00 00 00',
    txDefaultSession: '02 10 01 00 00 00 00 00'
  }
];

const UDS_NRC_DESCRIPTIONS = {
  0x10: 'General Reject',
  0x11: 'Service Not Supported',
  0x12: 'Sub-Function Not Supported',
  0x13: 'Incorrect Message Length or Invalid Format',
  0x21: 'Busy Repeat Request',
  0x22: 'Conditions Not Correct',
  0x24: 'Request Sequence Error',
  0x31: 'Request Out of Range',
  0x33: 'Security Access Denied',
  0x72: 'General Programming Failure',
  0x78: 'Request Correctly Received - Response Pending',
  0x7E: 'Sub-Function Not Supported In Active Session',
  0x7F: 'Service Not Supported In Active Session'
};

let actuatorStates = {};
let actuatorGuardTimers = {};
let actuatorPulseUiTimers = {};
let outputsLastVoltage = 12.6;
let outputsLastRpm = 0;
let absOutputsLastVoltage = 12.6;
let absOutputsFrontWheelKmh = 0.0;
let absOutputsRearWheelKmh = 0.0;

function findAnyActuatorByDid(did) {
  const norm = String(did || '').replace(/^0x/i, '').toUpperCase().padStart(4, '0');
  return ABS_OUTPUT_ACTUATORS.find(a => a.did === norm);
}

function isAbsVehicleMoving() {
  if (absOutputsFrontWheelKmh > 0.0 || absOutputsRearWheelKmh > 0.0) return true;
  if (latestAbs0200Data) {
    if (Number(latestAbs0200Data.front_wheel_speed || 0) > 0.0) return true;
    if (Number(latestAbs0200Data.rear_wheel_speed || 0) > 0.0) return true;
    if (Number(latestAbs0200Data.rear_right_wheel_speed || 0) > 0.0) return true;
    if (Number(latestAbs0200Data.vehicle_speed || 0) > 0.0) return true;
  }
  if (latestEcm0200Data && Number(latestEcm0200Data.vehicle_speed || 0) > 0.0) {
    return true;
  }
  return false;
}

function getMaxAbsWheelSpeedKmh() {
  let maxSpd = Math.max(absOutputsFrontWheelKmh || 0, absOutputsRearWheelKmh || 0);
  if (latestAbs0200Data) {
    maxSpd = Math.max(
      maxSpd,
      Number(latestAbs0200Data.front_wheel_speed || 0),
      Number(latestAbs0200Data.rear_wheel_speed || 0),
      Number(latestAbs0200Data.rear_right_wheel_speed || 0),
      Number(latestAbs0200Data.vehicle_speed || 0)
    );
  }
  if (latestEcm0200Data) {
    maxSpd = Math.max(maxSpd, Number(latestEcm0200Data.vehicle_speed || 0));
  }
  return maxSpd;
}

function clearAllActuatorTimers() {
  Object.keys(actuatorGuardTimers).forEach(k => {
    if (actuatorGuardTimers[k]) clearTimeout(actuatorGuardTimers[k]);
  });
  Object.keys(actuatorPulseUiTimers).forEach(k => {
    if (actuatorPulseUiTimers[k]) clearTimeout(actuatorPulseUiTimers[k]);
  });
  actuatorGuardTimers = {};
  actuatorPulseUiTimers = {};
}

function showActuatorAlertBanner(msgText, bannerType, isAbsTarget) {
  const useAbs = isAbsTarget !== undefined ? isAbsTarget : isAbsActiveModule();
  const bannerEl = document.getElementById(useAbs ? 'abs-actuator-status-banner' : 'actuator-status-banner')
    || document.getElementById('actuator-status-banner');
  if (!bannerEl) return;
  bannerEl.className = 'actuator-banner ' + (bannerType || 'banner-error');
  bannerEl.classList.remove('hidden');
  const canLabel = useAbs ? 'CAN 0x7E1 &middot; SID 0x2F' : 'CAN 0x7E0 &middot; SID 0x2F';
  bannerEl.innerHTML = `
    <span>${msgText}</span>
    <span class="mono" style="font-size:11px;opacity:0.85;">${canLabel}</span>
  `;
}

function showBatteryWarningBanner(volts, msgText) {
  const battBanner = document.getElementById('actuator-battery-banner');
  if (!battBanner) return;
  if (volts > 0.5 && volts < 11.8) {
    battBanner.classList.remove('hidden');
    battBanner.innerHTML = `
      <span>&#9888; ${msgText || ('LOW BATTERY SAFEGUARD: Battery voltage dropped to ' + Number(volts).toFixed(1) + 'V (< 11.8V) during service testing!')}</span>
      <span class="mono">${Number(volts).toFixed(2)} V</span>
    `;
  } else {
    battBanner.classList.add('hidden');
  }
}

function updateAbsOutputsHeaderTelemetry(volts, frontKmh, rearKmh) {
  if (volts !== undefined && volts !== null && !isNaN(Number(volts)) && Number(volts) > 0) {
    absOutputsLastVoltage = Number(volts);
  }
  if (frontKmh !== undefined && frontKmh !== null && !isNaN(Number(frontKmh))) {
    absOutputsFrontWheelKmh = Number(frontKmh);
  }
  if (rearKmh !== undefined && rearKmh !== null && !isNaN(Number(rearKmh))) {
    absOutputsRearWheelKmh = Number(rearKmh);
  }

  const maxKmh = getMaxAbsWheelSpeedKmh();
  const moving = maxKmh > 0.0;

  const speedPill = document.getElementById('abs-outputs-speed-pill');
  if (speedPill) {
    speedPill.className = 'ecm-state-pill ' + (moving ? 'state-red' : 'state-green');
    speedPill.textContent = moving
      ? `Disabled: Vehicle is Moving (${maxKmh.toFixed(1)} km/h)`
      : `SPEED: 0.0 km/h (STATIONARY)`;
  }

  const battPill = document.getElementById('abs-outputs-batt-pill');
  if (battPill) {
    const isLow = absOutputsLastVoltage < 11.8;
    battPill.className = 'ecm-state-pill ' + (isLow ? 'state-red' : 'state-green');
    battPill.textContent = `ABS VOLTS: ${absOutputsLastVoltage.toFixed(1)} V`;
  }

  const speedBanner = document.getElementById('abs-speed-interlock-banner');
  if (speedBanner) {
    speedBanner.classList.toggle('hidden', !moving);
    if (moving) {
      speedBanner.innerHTML = `
        <span>&#9888; Disabled: Vehicle is Moving (Wheel Speed ${maxKmh.toFixed(1)} km/h &gt; 0 km/h)</span>
        <span class="mono">0x7E1 SAFETY LOCK</span>
      `;
    }
  }

  // Update ABS Hydraulic Pump button (0x0540) state if vehicle speed lock changes
  if (document.getElementById('act-btn-0540')) {
    const cur0540 = actuatorStates['0540'] || { on: false, pulsing: false, loading: false, error: false };
    updateActuatorRowUi('0540', cur0540);
  }
}

function updateOutputsHeaderTelemetry(volts, rpm) {
  if (volts !== undefined && volts !== null && !isNaN(Number(volts)) && Number(volts) > 0) {
    outputsLastVoltage = Number(volts);
  }
  if (rpm !== undefined && rpm !== null && !isNaN(Number(rpm))) {
    outputsLastRpm = Number(rpm);
  }
  const vBadge = document.getElementById('outputs-batt-pill');
  if (vBadge) {
    const isLow = outputsLastVoltage < 11.8;
    vBadge.className = 'ecm-state-pill ' + (isLow ? 'state-red' : 'state-green');
    vBadge.textContent = `BATT: ${outputsLastVoltage.toFixed(1)} V`;
  }
  const rpmBadge = document.getElementById('outputs-rpm-pill');
  if (rpmBadge) {
    const isRunning = outputsLastRpm > 0;
    rpmBadge.className = 'ecm-state-pill ' + (isRunning ? 'state-red' : 'state-green');
    rpmBadge.textContent = `RPM: ${outputsLastRpm.toFixed(0)}`;
  }
  showBatteryWarningBanner(outputsLastVoltage);
}

function updateActuatorRowUi(did, stateInfo) {
  const actuator = findAnyActuatorByDid(did);
  if (!actuator) return;

  const isAbsPump = actuator.did === '0540';
  const vehicleMoving = isAbsPump && isAbsVehicleMoving();

  const rowEl = document.getElementById(`actuator-row-${actuator.did}`);
  const btnEl = document.getElementById(`actuator-btn-${actuator.did}`);
  const pillEl = document.getElementById(`actuator-pill-${actuator.did}`);
  const frameEl = document.getElementById(`actuator-frame-${actuator.did}`);

  if (rowEl) {
    rowEl.classList.toggle('is-active', !!(stateInfo.loading || stateInfo.pulsing || stateInfo.on));
    rowEl.classList.toggle('is-error', !!(stateInfo.error || vehicleMoving));
  }

  if (frameEl && stateInfo.lastFrame) {
    frameEl.textContent = `TX: ${stateInfo.lastFrame}`;
  }

  if (pillEl) {
    if (vehicleMoving) {
      pillEl.className = 'ecm-state-pill state-red';
      pillEl.textContent = 'Disabled: Vehicle is Moving';
    } else if (stateInfo.loading) {
      pillEl.className = 'ecm-state-pill state-orange';
      pillEl.textContent = isAbsPump ? 'AWAITING ABS (0x7E9)...' : 'AWAITING ECU...';
    } else if (stateInfo.pulsing) {
      pillEl.className = 'ecm-state-pill state-orange';
      pillEl.textContent = isAbsPump ? 'PUMP PULSING (1s)' : (actuator.mode === 'pulse_2s' ? 'PULSING (2s)' : 'FIRING PULSE');
    } else if (stateInfo.on) {
      pillEl.className = 'ecm-state-pill state-orange';
      pillEl.textContent = 'ACTIVE (ON)';
    } else if (stateInfo.error) {
      pillEl.className = 'ecm-state-pill state-red';
      pillEl.textContent = stateInfo.errorTag || 'TIMEOUT / ERR';
    } else {
      pillEl.className = 'ecm-state-pill state-green';
      pillEl.textContent = 'ECU CONTROL (IDLE)';
    }
  }

  if (btnEl) {
    if (vehicleMoving) {
      btnEl.disabled = true;
      btnEl.classList.remove('active-on');
      btnEl.innerHTML = `<span>Disabled: Vehicle is Moving</span>`;
    } else if (stateInfo.loading) {
      btnEl.disabled = true;
      btnEl.classList.remove('active-on');
      btnEl.innerHTML = `<span class="actuator-spinner"></span><span>Sending...</span>`;
    } else if (stateInfo.pulsing) {
      btnEl.disabled = true;
      btnEl.classList.add('active-on');
      const pulseLbl = isAbsPump ? 'Testing Pump (1s)...' : (actuator.mode === 'pulse_2s' ? 'Pulsing (2s)...' : 'Testing...');
      btnEl.innerHTML = `<span class="actuator-spinner"></span><span>${pulseLbl}</span>`;
    } else if (actuator.mode === 'toggle') {
      btnEl.disabled = false;
      btnEl.classList.toggle('active-on', !!stateInfo.on);
      btnEl.innerHTML = `<span>${stateInfo.on ? 'Toggle (ON)' : 'Toggle'}</span>`;
    } else {
      btnEl.disabled = false;
      btnEl.classList.remove('active-on');
      btnEl.innerHTML = `<span>${actuator.btnText}</span>`;
    }
  }
}

function handleActuatorWsResponse(msg) {
  if (!msg) return;
  const did = String(msg.did || '').replace(/^0x/i, '').toUpperCase().padStart(4, '0');
  const actuator = findAnyActuatorByDid(did);
  if (!actuator) return;

  const isAbs = (did === '0540' || String(msg.module || '').toLowerCase() === 'abs');

  // Clear the 3000ms frontend guard timer immediately upon receiving ECU response
  if (actuatorGuardTimers[did]) {
    clearTimeout(actuatorGuardTimers[did]);
    actuatorGuardTimers[did] = null;
  }

  const actuatorState = actuatorStates[did] || { on: false, pulsing: false, loading: false, error: false };
  actuatorState.loading = false;
  if (msg.tx_hex) {
    actuatorState.lastFrame = msg.tx_hex;
  }

  // Case 1: Automatic asynchronous pulse reset completed on ESP32 (04 2F <DID> 00 00 00 00)
  if (msg.auto_reset) {
    if (actuatorPulseUiTimers[did]) {
      clearTimeout(actuatorPulseUiTimers[did]);
      actuatorPulseUiTimers[did] = null;
    }
    actuatorState.on = false;
    actuatorState.pulsing = false;
    actuatorState.error = false;
    actuatorState.lastFrame = msg.tx_hex || actuator.txReset;
    actuatorStates[did] = actuatorState;
    updateActuatorRowUi(did, actuatorState);
    showActuatorAlertBanner(
      `${actuator.label} (${actuator.didHex}): 1s Pulse complete -> Returned Control to ECU [${msg.tx_hex || actuator.txReset}]`,
      'banner-ok',
      isAbs
    );
    return;
  }

  // Case 2: ECU did not reply (Timeout on CAN bus)
  if (!msg.got_reply && !msg.is_positive && !msg.is_negative) {
    actuatorState.on = false;
    actuatorState.pulsing = false;
    actuatorState.error = true;
    actuatorState.errorTag = isAbs ? 'ABS TIMEOUT' : 'NO RESPONSE';
    actuatorStates[did] = actuatorState;
    updateActuatorRowUi(did, actuatorState);
    showActuatorAlertBanner(
      isAbs ? 'ABS Module Communication Timeout' : 'Communication Timeout - No Response from ECU',
      'banner-timeout',
      isAbs
    );
    return;
  }

  // Case 3: Negative Response (03 7F 2F [NRC]) or Wheel Speed / RPM Safety Interlock
  if (msg.is_negative || msg.rpm_interlock || msg.speed_interlock) {
    actuatorState.on = false;
    actuatorState.pulsing = false;
    actuatorState.error = true;
    const nrcCode = Number(msg.nrc || 0x22);
    const nrcHex = nrcCode.toString(16).toUpperCase().padStart(2, '0');
    const nrcDesc = UDS_NRC_DESCRIPTIONS[nrcCode] || 'Conditions Not Correct';

    if (msg.speed_interlock) {
      actuatorState.errorTag = 'Disabled: Vehicle is Moving';
      actuatorStates[did] = actuatorState;
      updateActuatorRowUi(did, actuatorState);
      showActuatorAlertBanner('Disabled: Vehicle is Moving (Wheel Speed > 0 km/h)', 'banner-error', isAbs);
      return;
    }

    actuatorState.errorTag = msg.rpm_interlock ? 'RPM INTERLOCK' : `NRC 0x${nrcHex} (${nrcDesc})`;
    actuatorStates[did] = actuatorState;
    updateActuatorRowUi(did, actuatorState);

    const nrcBannerMsg = isAbs
      ? `ABS Negative Response (03 7F 2F ${nrcHex}): ${nrcDesc}`
      : (msg.message || `ECU Negative Response (03 7F 2F ${nrcHex}): ${nrcDesc}`);
    showActuatorAlertBanner(nrcBannerMsg, 'banner-error', isAbs);
    return;
  }

  // Case 4: Positive Response (0x6F)
  actuatorState.error = false;
  if (msg.state === 2 || (actuator.mode !== 'toggle' && msg.requested_state === 1)) {
    actuatorState.pulsing = true;
    actuatorState.on = true;
    const pulseMs = Number(msg.pulse_ms) || actuator.pulseMs || (isAbs ? 1000 : 1200);
    showActuatorAlertBanner(
      `${actuator.label} (${actuator.didHex}): Active [${msg.tx_hex || actuator.txOn}] -> Auto-reset in ${pulseMs} ms [${actuator.txReset}]`,
      'banner-ok',
      isAbs
    );
    if (actuatorPulseUiTimers[did]) clearTimeout(actuatorPulseUiTimers[did]);
    actuatorPulseUiTimers[did] = setTimeout(() => {
      const actuatorState = actuatorStates[did];
      if (actuatorState && actuatorState.pulsing) {
        actuatorState.pulsing = false;
        actuatorState.on = false;
        actuatorState.lastFrame = actuator.txReset;
        updateActuatorRowUi(did, actuatorState);
        showActuatorAlertBanner(
          `${actuator.label} (${actuator.didHex}): Returned Control to ECU [${actuator.txReset}]`,
          'banner-ok',
          isAbs
        );
      }
    }, pulseMs + 450);
  } else if (msg.state === 1) {
    actuatorState.pulsing = false;
    actuatorState.on = true;
    showActuatorAlertBanner(
      `${actuator.label} (${actuator.didHex}): Toggled ON [${msg.tx_hex || actuator.txOn}] (0x6F Confirmed)`,
      'banner-ok',
      isAbs
    );
  } else {
    actuatorState.pulsing = false;
    actuatorState.on = false;
    showActuatorAlertBanner(
      `${actuator.label} (${actuator.didHex}): Returned to ECU [${msg.tx_hex || actuator.txOff || actuator.txReset}] (0x6F Confirmed)`,
      'banner-ok',
      isAbs
    );
  }

  actuatorStates[did] = actuatorState;
  updateActuatorRowUi(did, actuatorState);
}

// Dedicated ABS Actuator Trigger (CAN ID 0x7E1 · Service 0x2F · DID 0x0540)
function triggerAbsActuatorCommand(did) {
  const normDid = String(did || '0540').replace(/^0x/i, '').toUpperCase().padStart(4, '0');
  const actuator = ABS_OUTPUT_ACTUATORS.find(actuatorEntry => actuatorEntry.did === normDid) || ABS_OUTPUT_ACTUATORS[0];
  if (!actuator) return;

  // 1. Wheel Speed Safety Lock (km/h > 0):
  //    Refuse execution of the pump actuator test if the vehicle is moving (km/h > 0)
  //    and disable the button on the UI dashboard with a warning notice ("Disabled: Vehicle is Moving").
  if (isAbsVehicleMoving()) {
    const lockState = actuatorStates[actuator.did] || { on: false, pulsing: false, loading: false, error: true };
    lockState.error = true;
    lockState.errorTag = 'Disabled: Vehicle is Moving';
    actuatorStates[actuator.did] = lockState;
    updateActuatorRowUi(actuator.did, lockState);
    showActuatorAlertBanner('Disabled: Vehicle is Moving (Wheel Speed > 0 km/h)', 'banner-error', true);
    return;
  }

  const actuatorState = actuatorStates[actuator.did] || { on: false, pulsing: false, loading: false, error: false };
  if (actuatorState.loading || actuatorState.pulsing) return;

  actuatorState.loading = true;
  actuatorState.error = false;
  actuatorState.lastFrame = actuator.txOn;
  actuatorStates[actuator.did] = actuatorState;
  updateActuatorRowUi(actuator.did, actuatorState);

  // 2. UI Timeout Guard (3000ms):
  //    Start a 3-second safeguard timer in web_src/main.js. If no response confirmation
  //    arrives within 3000ms, reset the button state and flash "ABS Module Communication Timeout".
  if (actuatorGuardTimers[actuator.did]) {
    clearTimeout(actuatorGuardTimers[actuator.did]);
  }
  actuatorGuardTimers[actuator.did] = setTimeout(() => {
    actuatorGuardTimers[actuator.did] = null;
    const actuatorState = actuatorStates[actuator.did];
    if (actuatorState && actuatorState.loading) {
      actuatorState.loading = false;
      actuatorState.pulsing = false;
      actuatorState.on = false;
      actuatorState.error = true;
      actuatorState.errorTag = 'ABS TIMEOUT (3s)';
      updateActuatorRowUi(actuator.did, actuatorState);
      showActuatorAlertBanner('ABS Module Communication Timeout', 'banner-timeout', true);
    }
  }, 3000);

  // 3. Send WebSocket Action Payload: {"action":"control_abs_output","did":"0540","state":1}
  const actionPayload = JSON.stringify({
    action: 'control_abs_output',
    module: 'abs',
    target_id: '0x7E1',
    did: actuator.did,
    state: 1
  });

  if (telemetryWs && telemetryWs.readyState === WebSocket.OPEN) {
    try {
      telemetryWs.send(actionPayload);
      return;
    } catch (e) {}
  }

  fetch(`/api/uds/outputs?id=0x7E1&module=abs&did=0x${encodeURIComponent(actuator.did)}&state=1`, {
    method: 'POST',
    cache: 'no-store'
  })
    .then(response => response.json())
    .then(data => {
      if (data && data.type === 'actuator_response') {
        handleActuatorWsResponse(data);
      }
    })
    .catch(() => {});
}

function triggerActuatorCommand(did) {
  // ECM output tests use ecmOutCmd(); this entry point only serves the ABS pump test.
  triggerAbsActuatorCommand(String(did || '').replace(/^0x/i, '').toUpperCase().padStart(4, '0'));
}

function cleanupAbsModuleOutputs(manualReset) {
  clearAllActuatorTimers();
  ABS_OUTPUT_ACTUATORS.forEach(actuator => {
    actuatorStates[actuator.did] = {
      on: false,
      pulsing: false,
      loading: false,
      error: false,
      lastFrame: actuator.txReset
    };
    updateActuatorRowUi(actuator.did, actuatorStates[actuator.did]);
  });

  // Failsafe Cleanup Routine for ABS (0x7E1):
  // Immediately sends UDS Reset frame (04 2F 05 40 00 00 00 00) + Default Session drop (02 10 01 00 00 00 00 00)
  // to 0x7E1 and stops the 2000ms 02 3E 80 00 00 00 00 00 TesterPresent heartbeat.
  if (telemetryWs && telemetryWs.readyState === WebSocket.OPEN) {
    try {
      telemetryWs.send(JSON.stringify({
        action: 'cleanup_abs_outputs',
        module: 'abs',
        target_id: '0x7E1',
        reset_frame: '04 2F 05 40 00 00 00 00',
        session_reset: '02 10 01 00 00 00 00 00'
      }));
    } catch (e) {}
  }
  fetch('/api/uds/outputs/cleanup?module=abs&id=0x7E1', { method: 'POST', keepalive: true }).catch(() => {});

  if (manualReset) {
    showActuatorAlertBanner(
      'ABS Failsafe Cleanup: Sent 04 2F 05 40 00 00 00 00 + Default Session (02 10 01 00 00 00 00 00) to 0x7E1',
      'banner-ok',
      true
    );
  }
}

function cleanupModuleOutputs(manualReset) {
  ecmSpcExit(false);
  if (isAbsActiveModule()) {
    cleanupAbsModuleOutputs(manualReset);
    return;
  }
  ecmOutExit(manualReset);
}

// ----------------------------------------------------------------------------
// WebSocket Receiver for Real-Time UDS Telemetry (DID 0x0200 & SID 0x2F)
// ----------------------------------------------------------------------------
function initTelemetryWebSocket() {
  if (!window.WebSocket) return;
  if (telemetryWs && (telemetryWs.readyState === WebSocket.OPEN || telemetryWs.readyState === WebSocket.CONNECTING)) {
    return;
  }

  try {
    const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${proto}//${window.location.host}/ws`;
    telemetryWs = new WebSocket(wsUrl);

    telemetryWs.onopen = () => {
      if (ecmStreamActive && currentActiveOperation === 'inputs') {
        const modKey = getActiveInputsModuleKey();
        if (isInputsKey(modKey)) {
          try {
            telemetryWs.send(JSON.stringify({ cmd: 'start_stream', module: modKey, id: activeModuleHex(), did: '0200' }));
          } catch (e) {}
        }
      } else if (currentActiveOperation === 'outputs') {
        try {
          if (isAbsActiveModule()) {
            telemetryWs.send(JSON.stringify({ action: 'enter_abs_outputs', module: 'abs', target_id: '0x7E1' }));
          } else if (isEcmActiveModule()) {
            telemetryWs.send(JSON.stringify({ action: 'enter_outputs' }));
          }
        } catch (e) {}
      }
    };

    telemetryWs.onmessage = (event) => {
      if (!event || !event.data) return;
      try {
        const msg = JSON.parse(event.data);
        const normalizedDid = String(msg.did || '').replace(/^0x/i, '').toUpperCase();
        const normalizedMod = String(msg.module || '').toLowerCase();

        if (msg.type === 'live_data' && normalizedDid === '0200' && msg.data) {
          if (msg.gen_layout !== undefined) {
            const genErr = document.getElementById('gen-inputs-error-banner');
            if (genErr) genErr.classList.add('hidden');
            updateGenInputs0200Dom(msg.data, msg.gen_layout);
          } else if (normalizedMod === 'abs') {
            const absErr = document.getElementById('abs-inputs-error-banner');
            if (absErr) absErr.classList.add('hidden');
            updateAbsInputs0200Dom(msg.data);
          } else if (normalizedMod === 'bcm') {
            const bcmErr = document.getElementById('bcm-inputs-error-banner');
            if (bcmErr) bcmErr.classList.add('hidden');
            updateBcmInputs0200Dom(msg.data);
          } else {
            const errBanner = document.getElementById('ecm-inputs-error-banner');
            if (errBanner) errBanner.classList.add('hidden');
            updateEcmInputs0200Dom(msg.data);
            if (msg.data.battery_voltage !== undefined) {
              updateOutputsHeaderTelemetry(msg.data.battery_voltage, msg.data.engine_speed);
            }
          }
        } else if (msg.type === 'ecm_out' || msg.type === 'bcm_out' || msg.type === 'abs_out') {
          const msgKind = msg.type === 'bcm_out' ? 'bcm' : (msg.type === 'abs_out' ? 'abs' : 'ecm');
          if (msg.event === 'status' && msgKind === outModuleKind()) ecmOutApplyStatus(msg);
        } else if (msg.type === 'actuator_response') {
          handleActuatorWsResponse(msg);
        } else if (msg.type === 'abs_outputs_telemetry') {
          updateAbsOutputsHeaderTelemetry(msg.battery_voltage, msg.front_wheel_speed, msg.rear_wheel_speed);
        } else if (msg.type === 'battery_warning') {
          updateOutputsHeaderTelemetry(msg.voltage, msg.rpm);
          showBatteryWarningBanner(msg.voltage, msg.message);
        } else if (msg.type === 'outputs_telemetry') {
          updateOutputsHeaderTelemetry(msg.battery_voltage, msg.engine_speed);
        } else if (msg.type === 'actuator_cleanup') {
          const isAbsClean = (normalizedMod === 'abs');
          showActuatorAlertBanner(
            msg.message || (isAbsClean
              ? 'ABS Hydraulic Pump Motor (0x0540) returned to ECU (04 2F 05 40 00 00 00 00) + Default Session (02 10 01)'
              : 'Returned all 7 actuators to ECU control (0x00) + Default Session (02 10 01)'),
            'banner-ok',
            isAbsClean
          );
        }
      } catch (err) {
        /* Ignore non-JSON frames */
      }
    };

    telemetryWs.onclose = () => {
      telemetryWs = null;
    };
    telemetryWs.onerror = () => {};
  } catch (e) {
    telemetryWs = null;
  }
}

function renderModuleOutputsView() {
  const container = document.getElementById('op-content-container');
  if (!container) return;

  clearAllActuatorTimers();

  // Branch 1: ABS Module (0x7E1) -> database-driven output tests (same page as ECM/BCM,
  // catalogue chosen from the ABS Calibration ID - see abs_outputs.h)
  if (isAbsActiveModule()) {
    ecmOut.base = '/api/abs/outputs';
    renderEcmOutputsView();
    return;
  }

  // Branch 1A: BCM (0x7E2) -> database-driven output tests (same page as the ECM)
  if (isBcmActiveModule()) {
    ecmOut.base = '/api/bcm/outputs';
    renderEcmOutputsView();
    return;
  }

  // Branch 1C: SPDO (0x7F2) -> database-driven output tests (same page as ECM/BCM/ABS - see spdo_outputs.h)
  if (isSpdoActiveModule()) {
    ecmOut.base = '/api/spdo/outputs';
    renderEcmOutputsView();
    return;
  }

  // Branch 1B: Non-ECM/BCM/ABS/SPDO Modules (e.g., LHC 0x7E3, RHC 0x7E5, RAD 0x7F1)
  if (!isEcmActiveModule()) {
    const modHex = currentActiveModule ? currentActiveModule.idHex : '0x7E2';
    const modAbbr = currentActiveModule ? (currentActiveModule.acronym || modHex) : 'BCM';
    container.innerHTML = `
      <div class="results-card" data-module="${modHex}">
        <div class="results-header-row">
          <span class="results-title">${modAbbr} Module Outputs</span>
        </div>
        <div style="padding:16px;background:rgba(245,158,11,0.08);border:1px solid rgba(245,158,11,0.28);border-radius:var(--radius-md);color:#fcd34d;font-size:12.5px;">
          Output tests are available for <strong>ECM (0x7E0)</strong>, <strong>BCM (0x7E2)</strong>, <strong>ABS (0x7E1)</strong> and <strong>SPDO (0x7F2)</strong>. Use <strong>Read Module Inputs</strong> on <strong>${modAbbr} (${modHex})</strong> to monitor live switch &amp; security states.
        </div>
      </div>
    `;
    return;
  }

  // Branch 2: ECM Module (CAN ID 0x7E0) -> database-driven output tests
  ecmOut.base = '/api/ecm/outputs';
  renderEcmOutputsView();
}

// ----------------------------------------------------------------------------
// ECM (0x7E0) OUTPUT TESTS - database-driven (firmware: ecm_outputs.cpp, /api/ecm/outputs/*)
// Modes: 1 = PULSE (fixed duration), 2 = ON/OFF, 3 = ADJUST (5 steps, engine running).
// ----------------------------------------------------------------------------
let ecmOut = { open: false, busy: false, cat: null, st: null, base: '/api/ecm/outputs' };
function outIsBcm() { return ecmOut.base === '/api/bcm/outputs'; }
function outIsAbs() { return ecmOut.base === '/api/abs/outputs'; }
function outIsSpdo() { return ecmOut.base === '/api/spdo/outputs'; }
function outModuleKind() { return outIsBcm() ? 'bcm' : (outIsAbs() ? 'abs' : (outIsSpdo() ? 'spdo' : 'ecm')); }

function ecmOutModeText(o) {
  if (o.mode === 1) return `Pulse &middot; ${o.delay} s`;
  if (o.mode === 3) return 'Adjustable &middot; engine running';
  return 'ON / OFF';
}

function ecmOutRowHtml(o) {
  let btns = '';
  if (o.mode === 1) {
    btns = `<button type="button" class="btn-actuator" data-op="fire" onclick="ecmOutCmd('${o.key}','fire',0)"><span>Run (${o.delay}s)</span></button>
            <button type="button" class="btn btn-secondary btn-xs" onclick="ecmOutCmd('${o.key}','stop',0)"><span>Stop</span></button>`;
  } else if (o.mode === 3) {
    btns = `<button type="button" class="btn-actuator" data-op="arm" onclick="ecmOutCmd('${o.key}','arm',0)"><span>Activate</span></button>`;
    o.steps.forEach((v, i) => {
      btns += `<button type="button" class="btn btn-secondary btn-xs" id="ecmout-step-${o.key}-${i}" onclick="ecmOutCmd('${o.key}','set',${i})"><span>${v}</span></button>`;
    });
    btns += `<button type="button" class="btn btn-secondary btn-xs" onclick="ecmOutCmd('${o.key}','stop',0)"><span>Stop</span></button>`;
  } else {
    btns = `<button type="button" class="btn-actuator" data-op="on" onclick="ecmOutCmd('${o.key}','on',0)"><span>ON</span></button>
            <button type="button" class="btn btn-secondary btn-xs" onclick="ecmOutCmd('${o.key}','off',0)"><span>OFF</span></button>`;
  }
  return `
    <div class="actuator-row-card out-row" id="ecmout-row-${o.key}">
      <div class="out-head">
        <div class="actuator-title">${o.name}</div>
        <span class="ecm-state-pill state-green" id="ecmout-pill-${o.key}">ECU CONTROL</span>
      </div>
      <div class="actuator-meta-line">
        <span>${ecmOutModeText(o)}${o.engine_off ? ' &middot; engine off' : ''}</span>
        ${o.secure ? '<span>&middot;</span><span>security-protected</span>' : ''}
      </div>
      <div class="out-btns${o.mode === 3 ? ' out-btns-steps' : ''}">${btns}</div>
    </div>`;
}

function ecmOutBuild() {
  const cat = ecmOut.cat;
  const list = document.getElementById('ecmout-list');
  if (!cat || !list) return;
  list.innerHTML = cat.outputs.length
    ? cat.outputs.map(ecmOutRowHtml).join('')
    : '<div class="empty-state"><p class="empty-text">The database defines no output tests for this calibration.</p></div>';
}

function ecmOutApplyStatus(st) {
  if (!st) return;
  ecmOut.st = st;
  const set = (id, cls, txt) => { const element = document.getElementById(id); if (element) { element.className = 'ecm-state-pill ' + cls; element.textContent = txt; } };
  set('ecmout-batt', (st.batt > 1 && st.batt < 11.8) ? 'state-red' : 'state-green', `BATT: ${Number(st.batt || 0).toFixed(1)} V`);
  set('ecmout-rpm', st.running ? 'state-red' : 'state-green', st.running ? `ENGINE RUNNING ${Number(st.rpm || 0).toFixed(0)} rpm` : `ENGINE OFF ${Number(st.rpm || 0).toFixed(0)} rpm`);
  set('ecmout-session', st.session ? (st.unlocked ? 'state-orange' : 'state-orange') : 'state-green',
      st.session ? (st.unlocked ? 'EXT SESSION · UNLOCKED' : 'EXT SESSION') : 'DEFAULT SESSION');
  const msgElement = document.getElementById('ecmout-msg');
  if (msgElement && st.msg) msgElement.textContent = st.msg;
  if (!ecmOut.cat) return;
  ecmOut.cat.outputs.forEach(outputDef => {
    const outputState = st.outputs ? st.outputs[outputDef.key] : null;
    const stateCode = outputState ? outputState.s : 0;
    const txt = stateCode === 2 ? 'RUNNING' : (stateCode === 1 ? (outputDef.mode === 3 ? `ACTIVE · ${outputDef.steps[outputState.v]}` : 'ON') : 'ECU CONTROL');
    set(`ecmout-pill-${outputDef.key}`, stateCode ? 'state-orange' : 'state-green', txt);
    if (outputDef.mode === 3) {
      outputDef.steps.forEach((_, stepIndex) => {
        const stepButton = document.getElementById(`ecmout-step-${outputDef.key}-${stepIndex}`);
        if (stepButton) stepButton.classList.toggle('btn-primary', stateCode === 1 && outputState.v === stepIndex);
      });
    }
  });
}

function ecmOutRefresh() {
  return fetch(ecmOut.base + '/status', { cache: 'no-store' }).then(response => response.json()).then(ecmOutApplyStatus).catch(() => {});
}

function renderEcmOutputsView() {
  const container = document.getElementById('op-content-container');
  if (!container) return;
  const kind = outModuleKind();
  const isBcmOrAbs = kind !== 'ecm';
  const modId = kind === 'bcm' ? '0x7E2' : (kind === 'abs' ? '0x7E1' : (kind === 'spdo' ? '0x7F2' : '0x7E0'));
  const modName = kind === 'bcm' ? 'BCM' : (kind === 'abs' ? 'ABS' : (kind === 'spdo' ? 'SPDO' : 'ECM'));
  container.innerHTML = `
    <div class="results-card" id="ecm-outputs-card" data-module="${modId}">
      <div class="results-header-row ecm-telemetry-toolbar">
        <div><span class="results-title">${modName} OUTPUT TESTS</span></div>
        <div class="tool-wrap">
          <span class="ecm-state-pill state-green" id="ecmout-batt">BATT: -- V</span>
          <span class="ecm-state-pill state-green" id="ecmout-rpm">ENGINE --</span>
          <span class="ecm-state-pill state-green" id="ecmout-session">DEFAULT SESSION</span>
          <button type="button" class="btn btn-secondary btn-xs" onclick="ecmOutExit(true)"><span>Return All to ECU</span></button>
        </div>
      </div>
      ${isBcmOrAbs ? '' : `<div class="actuator-banner banner-ok neutral-banner">
        <label>
          <input type="checkbox" id="ecmout-neutral"> <span>Gearbox in neutral, bike secured on a stand &mdash; required to start a test</span>
        </label>
      </div>`}
      <div id="ecmout-msg" class="actuator-banner banner-ok">Loading output tests from ${modName}...</div>
      <div class="actuator-list-wrap" id="ecmout-list"></div>
    </div>`;

  initTelemetryWebSocket();
  ecmOut.open = true;
  fetch(ecmOut.base, { cache: 'no-store' })
    .then(response => response.json())
    .then(cat => { ecmOut.cat = cat; ecmOutBuild(); return ecmOutRefresh(); })
    .catch(() => { const msgElement = document.getElementById('ecmout-msg'); if (msgElement) msgElement.textContent = 'Could not load the output tests.'; });
}

function ecmOutBusy(busyState) {
  ecmOut.busy = busyState;
  document.querySelectorAll('#ecmout-list button').forEach(buttonElement => { buttonElement.disabled = busyState; });
}

async function ecmOutCmd(key, op, value) {
  if (ecmOut.busy) return;
  const neutralCheckbox = document.getElementById('ecmout-neutral');
  const neutral = (outIsBcm() || outIsAbs() || outIsSpdo() || (neutralCheckbox && neutralCheckbox.checked)) ? 1 : 0;
  if ((outIsBcm() || outIsAbs()) && (op === 'on' || op === 'fire') && ecmOut.cat) {
    const outputDef = ecmOut.cat.outputs.find(outputEntry => outputEntry.key === key);
    if (outputDef && outputDef.warn && !(await hdConfirm(outputDef.warn, outputDef.name))) return;
  }
  ecmOutBusy(true);
  fetch(`${ecmOut.base}/cmd?key=${encodeURIComponent(key)}&op=${op}&value=${value}&neutral=${neutral}`, { method: 'POST', cache: 'no-store' })
    .then(response => response.json())
    .then(cmdResult => {
      const msgElement = document.getElementById('ecmout-msg');
      if (msgElement) { msgElement.textContent = cmdResult.msg || ''; msgElement.className = 'actuator-banner ' + (cmdResult.ok ? 'banner-ok' : 'banner-error'); }
    })
    .catch(() => {
      const msgElement = document.getElementById('ecmout-msg');
      if (msgElement) { msgElement.textContent = 'No response from the ESP32'; msgElement.className = 'actuator-banner banner-error'; }
    })
    .then(() => { ecmOutBusy(false); return ecmOutRefresh(); });
}

function ecmOutExit(manual) {
  if (!ecmOut.open && !manual) return;
  ecmOut.open = false;
  fetch(ecmOut.base + '/exit' + (manual ? '?keep=1' : ''), { method: 'POST', keepalive: true })
    .then(() => { if (manual) { ecmOut.open = !!document.getElementById('ecm-outputs-card'); return ecmOutRefresh(); } })
    .catch(() => {});
  if (manual) {
    const msgElement = document.getElementById('ecmout-msg');
    if (msgElement) { msgElement.textContent = 'Control returned to ECU, default session restored'; msgElement.className = 'actuator-banner banner-ok'; }
  }
}

function renderSpecialFunctionsView() {
  const container = document.getElementById('op-content-container');
  if (!container) return;
  if (isBcmActiveModule()) { ecmSpc.base = '/api/bcm/special'; renderEcmSpecialView(); return; }
  if (isAbsActiveModule()) { ecmSpc.base = '/api/abs/special'; renderEcmSpecialView(); return; }
  if (isSpdoActiveModule()) { ecmSpc.base = '/api/spdo/special'; renderEcmSpecialView(); return; }
  if (isRadActiveModule()) { ecmSpc.base = '/api/rad/special'; renderEcmSpecialView(); return; }
  if (isLhcActiveModule()) { ecmSpc.base = '/api/lhc/special'; renderEcmSpecialView(); return; }
  if (isEcmActiveModule()) { ecmSpc.base = '/api/ecm/special'; renderEcmSpecialView(); return; }
  container.innerHTML = `
    <div class="results-card">
      <div class="results-header-row"><span class="results-title">Service &amp; Calibration Routines</span></div>
      <div class="dtc-item-card"><span>Key Fob Security PIN Setup</span><button class="btn btn-secondary">Configure</button></div>
      <div class="dtc-item-card"><span>ABS Brake Hydraulic Bleed</span><button class="btn btn-secondary">Start Bleed</button></div>
      <div class="dtc-item-card"><span>Speedometer Pulse Calibration</span><button class="btn btn-secondary">Calibrate</button></div>
    </div>
  `;
}

// ----------------------------------------------------------------------------
// ECM special functions (database driven, /api/ecm/special/*)
// ----------------------------------------------------------------------------
const ecmSpc = { open: false, ended: false, cat: null, busy: false, poll: null, cur: {}, base: '/api/ecm/special' };
// Display label for the BCM Boost function. Defined only in the firmware (BCM_BOOST_LABEL in
// bcm_security.h) and sent in the BCM special-function catalog as "boost_label".
function boostLabel() { return (ecmSpc.cat && ecmSpc.cat.boost_label) || ''; }

function spcEsc(s) {
  return String(s == null ? '' : s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

function spcMsg(text, ok) {
  const msgElement = document.getElementById('ecmspc-msg');
  if (msgElement) { msgElement.textContent = text || ''; msgElement.className = 'actuator-banner ' + (ok === false ? 'banner-error' : 'banner-ok'); }
}

function spcIsBcm() { return ecmSpc.base === '/api/bcm/special'; }
function spcIsAbs() { return ecmSpc.base === '/api/abs/special'; }
function spcIsSpdo() { return ecmSpc.base === '/api/spdo/special'; }
function spcIsRad() { return ecmSpc.base === '/api/rad/special'; }
function spcIsLhc() { return ecmSpc.base === '/api/lhc/special'; }
function spcIsEcm() { return ecmSpc.base === '/api/ecm/special'; }
function spcModuleName() { return spcIsBcm() ? 'BCM' : (spcIsAbs() ? 'ABS' : (spcIsSpdo() ? 'SPDO' : (spcIsRad() ? 'RAD' : (spcIsLhc() ? 'LHC' : 'ECM')))); }

function renderEcmSpecialView() {
  const container = document.getElementById('op-content-container');
  if (!container) return;
  const modName = spcModuleName();
  const modId = spcIsBcm() ? '0x7E2' : (spcIsAbs() ? '0x7E1' : (spcIsSpdo() ? '0x7F2' : (spcIsRad() ? '0x7F1' : (spcIsLhc() ? '0x7E3' : '0x7E0'))));
  container.innerHTML = `
    <div class="results-card" id="ecm-special-card" data-module="${modId}">
      <div class="results-header-row ecm-telemetry-toolbar">
        <div><span class="results-title">${modName} SPECIAL FUNCTIONS</span></div>
        <div class="tool-wrap">
          <span class="ecm-state-pill state-green" id="ecmspc-cal">CAL --</span>
        </div>
      </div>
      <div id="ecmspc-msg" class="actuator-banner banner-ok">Loading special functions from the ${modName}...</div>
      <div class="actuator-list-wrap" id="ecmspc-list"></div>
    </div>`;
  ecmSpc.open = true;
  ecmSpc.ended = false;
  ecmSpcEnter();
}

// Fetches the special-function catalogue. Each operation opens and closes its own extended session on the
// module side (atomic), so there is no persistent session to start or stop here - just load the list.
function ecmSpcEnter() {
  const modName = spcModuleName();
  ecmSpc.cur = {};
  spcMsg(`Loading special functions from the ${modName}...`, true);
  fetch(ecmSpc.base, { cache: 'no-store' })
    .then(response => response.json())
    .then(cat => { ecmSpc.cat = cat; ecmSpcBuild(); })
    .catch(() => spcMsg(`Could not load the ${modName} special functions.`, false));
}

// Per-module guided procedures (type 9), injected client-side so each module's catalog stays untouched.
// Each entry drives the shared /api/proc/* engine (proc_tables.h). ABS already ships its own from firmware.
const PROC_FUNCTIONS = {
  // quiet:true = tested & trusted procedures - no "runs a live procedure / compare against your diagnostic tool" banner,
  // no start-confirmation popup and no completion popup; the on-screen steps and the inline status line remain.
  '/api/abs/special': [
    { key: 'abs_complete', proc: 'abs_complete', name: 'ABS: complete installation', type: 9, delay: 0, quiet: true,
      desc: 'Clears the ABS service-required flag after a replacement and bleed. Run the bleed first.' }
  ],
  // TPS reset, Clear adaptations and VIN write now have their own custom cards (Run / Read+Write with their
  // own Run button, like BCM boost/FOB), injected directly into the ECM special-function list below - no
  // guided-procedure buttons here.
  '/api/ecm/special': [],
  // "spdo_pair" / "spdo_pair_vin" used to be guided procedures here. Both are now folded into the two
  // standalone cards below (Identity blocks Read/Save/Write/Load, and VIN write) - no guided-procedure
  // buttons needed for SPDO any more.
  '/api/spdo/special': [],
  // BCM key-FOB assignment now has its own PIN-style cards (read current + editable hex field + write),
  // injected directly into the BCM special-function list below - no guided-procedure buttons here.
  '/api/bcm/special': []
  // LHC: the Cruise / Push-to-Talk function is a single config option (read/apply/restore) served by the
  // /api/lhc/special catalog itself (type 6), not a guided procedure - so no PROC_FUNCTIONS entry here.
};
function injectProcFunctions(cat) {
  const extra = PROC_FUNCTIONS[ecmSpc.base];
  if (!extra || !cat) return;
  cat.functions = cat.functions || [];
  extra.forEach(e => { if (!cat.functions.some(f => f.key === e.key)) cat.functions.push(e); });
}

function ecmSpcBuild() {
  const cat = ecmSpc.cat;
  if (!cat) return;
  injectProcFunctions(cat);
  const calEl = document.getElementById('ecmspc-cal');
  if (calEl) calEl.textContent = ((spcIsBcm() || spcIsSpdo() || spcIsRad()) ? 'P/N ' : (spcIsAbs() ? '' : 'CAL ')) + (cat.cal || (spcIsAbs() ? 'all calibrations' : '--'));
  const list = document.getElementById('ecmspc-list');
  if (!list) return;
  if (!cat.functions || !cat.functions.length) {
    list.innerHTML = '<div class="actuator-banner banner-error">The database defines no special functions for this calibration.</div>';
    spcMsg('No special functions for calibration ' + (cat.cal || '?'), false);
    return;
  }
  spcMsg('Special functions ready (profile ' + cat.profile_name + '). Tap a function to open it — it reads its current state only then.', true);
  let rawHtml = '';     // custom cards shown BEFORE the database-driven function list
  let rawHtmlEnd = '';  // custom cards shown AFTER it (e.g. a module's VIN write card)
  if (spcIsBcm()) {
    rawHtml = `
      <div class="actuator-row-card spc-fn" id="spc-boost">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('boost',99)"><span><strong>${boostLabel()}</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-boost-body" style="display:none">
          <div class="spc-state" id="spc-boost-status">Current: not read</div>
          <div class="spc-fields"><label>New value<input type="text" class="input-dark" id="spc-boost-val" placeholder="-----" maxlength="5" inputmode="numeric" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false" oninput="pinValueInput(this)"></label></div>
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="ecmSpcReadBoost()"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" onclick="ecmSpcApplyBoost()"><span>Apply</span></button>
            <button class="btn btn-secondary btn-xs" onclick="ecmSpcRestoreBoost()"><span>Restore original</span></button>
          </div>
        </div>
      </div>
      <div class="actuator-row-card spc-fn" id="spc-fobread">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('fobread',98)"><span><strong>Key FOB status</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-fobread-body" style="display:none">
          <div class="spc-state" id="fob-read-result">Reading...</div>
          <div class="spc-btns"><button class="btn btn-secondary btn-xs" onclick="fobStatusRead()"><span>Read</span></button></div>
        </div>
      </div>
      <div class="actuator-row-card spc-fn" id="spc-fob1">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('fob1',96)"><span><strong>Key FOB assignment (1 FOB)</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-fob1-body" style="display:none">
          <div class="spc-state" id="fob1-cur">Current: not read</div>
          <div class="spc-fields"><label>New FOB serial<input type="text" class="input-dark" id="fob1-in" placeholder="XXXX-XXXX-XXXX-XXXX-XXXX-XXXX" maxlength="29" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false" oninput="fobSerialInput(this)"></label></div>
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="fob1Read()"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" onclick="fob1Assign()"><span>Assign</span></button>
            <button class="btn btn-secondary btn-xs" onclick="fobRestore(1)"><span>Restore original</span></button>
          </div>
        </div>
      </div>
      <div class="actuator-row-card spc-fn" id="spc-fob2">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('fob2',95)"><span><strong>Key FOB assignment (2 FOBs)</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-fob2-body" style="display:none">
          <div class="spc-state" id="fob2a-cur">FOB 1 current: not read</div>
          <div class="spc-fields"><label>New FOB 1 serial<input type="text" class="input-dark" id="fob2a-in" placeholder="XXXX-XXXX-XXXX-XXXX-XXXX-XXXX" maxlength="29" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false" oninput="fobSerialInput(this)"></label></div>
          <div class="spc-state" id="fob2b-cur">FOB 2 current: not read</div>
          <div class="spc-fields"><label>New FOB 2 serial<input type="text" class="input-dark" id="fob2b-in" placeholder="XXXX-XXXX-XXXX-XXXX-XXXX-XXXX" maxlength="29" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false" oninput="fobSerialInput(this)"></label></div>
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="fob2Read()"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" onclick="fob2Assign()"><span>Assign</span></button>
            <button class="btn btn-secondary btn-xs" onclick="fobRestore(2)"><span>Restore original</span></button>
          </div>
        </div>
      </div>`;
    // No VIN write card for BCM - the BCM has no VIN DID, unlike ECM/ABS/SPDO/Radio (confirmed, so the card
    // that used to appear here was dropped rather than left to fail against a DID the module doesn't have).
  } else if (spcIsAbs()) {
    // ABS otherwise has only the two guided bleed procedures (nothing with a readable current value) - add
    // the VIN card the same way ECM's was added, same DID the Read ECU Information screen uses (0xF1EF).
    // Shown AFTER the bleed procedures (rawHtmlEnd), same placement as the other modules' VIN cards.
    rawHtmlEnd = `
      <div class="actuator-row-card spc-fn" id="spc-abs-vin">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('abs-vin',89)"><span><strong>VIN write (ABS)</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-abs-vin-body" style="display:none">
          <div class="spc-state" id="abs-vin-cur">Current VIN: not read</div>
          <div class="spc-fields"><label>New VIN (17 characters)<input type="text" class="input-dark" id="abs-vin-in" placeholder="17-character VIN" maxlength="17" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false"></label></div>
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="vinCardRead('abs-vin')"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" onclick="vinCardWrite('abs-vin')"><span>Write</span></button>
          </div>
          <div class="spc-hint">Used after an ABS module swap to restore the original VIN.</div>
        </div>
      </div>`;
  } else if (spcIsSpdo()) {
    rawHtml = `
      <div class="actuator-row-card spc-fn" id="spc-vehinfo">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('vehinfo',97)"><span><strong>Odometer &amp; VIN</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-vehinfo-body" style="display:none">
          <div class="spc-state" id="veh-odo">Odometer: reading...</div>
          <div class="spc-state" id="veh-vin">VIN: &mdash;</div>
          <div class="spc-btns"><button class="btn btn-secondary btn-xs" onclick="vehicleInfoRead()"><span>Read</span></button></div>
        </div>
      </div>`;
    // Both SPDO pairing methods consolidated into these two cards, shown AFTER the database-driven function
    // list (rawHtmlEnd), same placement as the other modules' VIN cards: "Identity blocks" now reads, saves,
    // loads and writes the 0157/0158 blocks from the one card (replacing the old "Speedometer pairing
    // (config)" guided procedure, which made the operator type the blocks in by hand); "VIN write" is the
    // same one-shot Read/Write action the other modules use (replacing "Speedometer pairing (VIN)").
    rawHtmlEnd = `
      <div class="actuator-row-card spc-fn" id="spc-spdo-idblocks">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('spdo-idblocks',91)"><span><strong>Identity blocks (0157/0158)</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-spdo-idblocks-body" style="display:none">
          <div class="spc-state" id="spdo-id-157">Block 1 (DID 0157): not read</div>
          <div class="spc-fields"><label>Block 1 (DID 0157, 24 hex digits)<input type="text" class="input-dark" id="spdo-id-157-in" placeholder="24 hex digits" maxlength="24" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false"></label></div>
          <div class="spc-state" id="spdo-id-158">Block 2 (DID 0158): not read</div>
          <div class="spc-fields"><label>Block 2 (DID 0158, 24 hex digits)<input type="text" class="input-dark" id="spdo-id-158-in" placeholder="24 hex digits" maxlength="24" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false"></label></div>
          <input type="file" id="spdo-id-file" accept=".txt" style="display:none" onchange="spdoIdLoadFile(this)">
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="spdoIdRead()"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" id="spdo-id-save" onclick="spdoIdSave()" disabled><span>Save</span></button>
            <button class="btn btn-secondary btn-xs" onclick="document.getElementById('spdo-id-file').click()"><span>Load</span></button>
            <button class="btn btn-primary btn-xs" onclick="spdoIdWrite()"><span>Write</span></button>
          </div>
          <div class="spc-hint">Read and Save from the OLD cluster before swapping it out; on the replacement, either Read (to check what's there) or Load the saved file, then Write - all from this one card, no external diagnostic tool needed.</div>
        </div>
      </div>
      <div class="actuator-row-card spc-fn" id="spc-spdo-vin">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('spdo-vin',90)"><span><strong>VIN write (Speedometer)</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-spdo-vin-body" style="display:none">
          <div class="spc-state" id="spdo-vin-cur">Current VIN: not read</div>
          <div class="spc-fields"><label>New VIN (17 characters)<input type="text" class="input-dark" id="spdo-vin-in" placeholder="17-character VIN" maxlength="17" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false"></label></div>
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="vinCardRead('spdo-vin')"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" onclick="vinCardWrite('spdo-vin')"><span>Write</span></button>
          </div>
          <div class="spc-hint">Alternative to the identity-block pairing above - writes the 17-char VIN directly (same DID the Read ECU Information screen uses, F1EF or F190 depending on calibration).</div>
        </div>
      </div>`;
  } else if (spcIsRad()) {
    // VIN write shown AFTER the database-driven function list, same placement as the other modules' VIN cards.
    rawHtmlEnd = `
      <div class="actuator-row-card spc-fn" id="spc-rad-vin">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('rad-vin',88)"><span><strong>VIN write (Radio)</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-rad-vin-body" style="display:none">
          <div class="spc-state" id="rad-vin-cur">Current VIN: not read</div>
          <div class="spc-fields"><label>New VIN (17 characters)<input type="text" class="input-dark" id="rad-vin-in" placeholder="17-character VIN" maxlength="17" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false"></label></div>
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="vinCardRead('rad-vin')"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" onclick="vinCardWrite('rad-vin')"><span>Write</span></button>
          </div>
          <div class="spc-hint">Used after a radio module swap to restore the original VIN. The write still needs the radio's captured seed/key (rad_special.cpp) to unlock - an unrecognised seed is reported so it can be captured.</div>
        </div>
      </div>`;
  } else if (spcIsEcm()) {
    // TPS reset, Clear adaptations and VIN write: one-shot UDS exchanges with nothing for the operator to do
    // mid-sequence, so plain Run / Read+Write actions here instead of the multi-step guided-procedure popup.
    // Shown AFTER the database-driven function list (rawHtmlEnd), not before it.
    rawHtmlEnd = `
      <div class="actuator-row-card spc-fn" id="spc-ecm-tps">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('ecm-tps',93)"><span><strong>TPS reset / throttle relearn</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-ecm-tps-body" style="display:none">
          <div class="spc-hint">Resets the throttle-position adaptation and relearns idle. Engine off, ignition on.</div>
          <div class="spc-btns"><button class="btn btn-primary btn-xs" onclick="ecmTpsResetRun()"><span>Run</span></button></div>
        </div>
      </div>
      <div class="actuator-row-card spc-fn" id="spc-ecm-relearn">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('ecm-relearn',92)"><span><strong>Clear adaptations + relearn</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-ecm-relearn-body" style="display:none">
          <div class="spc-hint">Clears stored adaptations so they relearn on the next start. Engine off, ignition on.</div>
          <div class="spc-btns"><button class="btn btn-primary btn-xs" onclick="ecmClearAdaptRun()"><span>Run</span></button></div>
        </div>
      </div>
      <div class="actuator-row-card spc-fn" id="spc-ecm-vin">
        <button type="button" class="spc-head" onclick="ecmSpcToggle('ecm-vin',94)"><span><strong>VIN write (ECM)</strong></span><span class="spc-caret">&#9656;</span></button>
        <div class="actuator-controls" id="spc-ecm-vin-body" style="display:none">
          <div class="spc-state" id="ecm-vin-cur">Current VIN: not read</div>
          <div class="spc-fields"><label>New VIN (17 characters)<input type="text" class="input-dark" id="ecm-vin-in" placeholder="17-character VIN" maxlength="17" inputmode="latin" autocomplete="off" autocorrect="off" autocapitalize="characters" spellcheck="false"></label></div>
          <div class="spc-btns">
            <button class="btn btn-secondary btn-xs" onclick="vinCardRead('ecm-vin')"><span>Read</span></button>
            <button class="btn btn-primary btn-xs" onclick="vinCardWrite('ecm-vin')"><span>Write</span></button>
          </div>
          <div class="spc-hint">Used after an ECM swap to restore the original VIN.</div>
        </div>
      </div>`;
  }
  // ABS has only guided procedures (nothing with a readable current value), so no Read all / Save all there.
  // LHC has a single function (Cruise / Push-to-Talk side) - not worth a bulk read/save bar either.
  const allBar = (spcIsAbs() || spcIsLhc()) ? '' : `
    <div class="spc-btns" id="spc-allbar" style="margin:0 0 8px">
      <button class="btn btn-secondary btn-xs" id="spc-readall" onclick="spcReadAll()"><span>Read all</span></button>
      <button class="btn btn-secondary btn-xs" id="spc-saveall" onclick="spcSaveAll()" disabled><span>Save all</span></button>
    </div>`;
  spcReadAllDone = false;
  list.innerHTML = allBar + rawHtml + cat.functions.map(funcDef => {
    const id = 'spc-' + funcDef.key;
    let body = '';
    if (funcDef.type === 8) {
      body = `<div class="spc-state" id="${id}-val">not read</div>
        <div class="spc-btns"><button class="btn btn-secondary btn-xs" onclick="ecmSpcTpmsConfigRead('${funcDef.key}')"><span>Read status</span></button></div>`;
    } else if (funcDef.type === 7) {
      body = `${funcDef.warn ? `<div class="spc-hint spc-warn">${spcEsc(funcDef.warn)}</div>` : ''}
        <div class="spc-state" id="${id}-val">not read</div>
        <div class="spc-fields">
          <label>Front sensor ID<input type="text" class="input-dark" id="${id}-front" placeholder="AA-BB-CC-DD" maxlength="11"></label>
          <label>Rear sensor ID<input type="text" class="input-dark" id="${id}-rear" placeholder="AA-BB-CC-DD" maxlength="11"></label>
        </div>
        <div class="spc-btns">
          <button class="btn btn-secondary btn-xs" onclick="ecmSpcTpmsSensorRead('${funcDef.key}')"><span>Read</span></button>
          <button class="btn btn-primary btn-xs" onclick="ecmSpcTpmsSensorWriteConfirm('${funcDef.key}')"><span>Assign</span></button>
        </div>
        <div class="spc-hint">Sensor ID is printed on the TPMS sensor body, as 4 hex byte pairs (e.g. AA-BB-CC-DD).</div>`;
    } else if (funcDef.type === 2) {
      body = `<div class="spc-state" id="${id}-val">state: not read</div>
        <div class="spc-btns">
        <button class="btn btn-secondary btn-xs" onclick="ecmSpcRead('${funcDef.key}')"><span>Read</span></button>
        <button class="btn btn-primary btn-xs" onclick="ecmSpcRun('${funcDef.key}',1)"><span>Enable</span></button>
        <button class="btn btn-primary btn-xs" onclick="ecmSpcRun('${funcDef.key}',0)"><span>Disable</span></button>
        <button class="btn btn-secondary btn-xs" onclick="ecmSpcRestore('${funcDef.key}')"><span>Restore original</span></button></div>`;
    } else if (funcDef.type === 6) {
      body = `${funcDef.warn ? `<div class="spc-hint spc-warn">${spcEsc(funcDef.warn)}</div>` : ''}
        <div class="spc-state" id="${id}-val">Current: not read</div>
        <div class="spc-fields"><label>New value<select class="input-dark" id="${id}-sel">${(funcDef.opts || []).map((t, i) => `<option value="${i}">${spcEsc(t)}</option>`).join('')}</select></label></div>
        <div class="spc-btns">
        <button class="btn btn-secondary btn-xs" onclick="ecmSpcRead('${funcDef.key}')"><span>Read</span></button>
        <button class="btn btn-primary btn-xs" onclick="ecmSpcApply('${funcDef.key}')"><span>Apply</span></button>
        <button class="btn btn-secondary btn-xs" onclick="ecmSpcRestore('${funcDef.key}')"><span>Restore original</span></button></div>`;
    } else if (funcDef.type === 1) {
      body = `<div class="spc-state" id="${id}-val">idle: not read</div>
        <div class="spc-hint" id="${id}-live"></div>
        <div class="spc-fields"><label>Idle speed (rpm)
          <div class="spc-stepper">
            <button type="button" class="btn btn-secondary spc-step-btn" onclick="ecmSpcIdleStep('${funcDef.key}',-1)" aria-label="decrease">&minus;</button>
            <input type="number" inputmode="numeric" class="input-dark" id="${id}-in" min="${funcDef.min}" max="${funcDef.max}" step="${funcDef.step}" value="952" onchange="ecmSpcIdleSnapField('${funcDef.key}')">
            <button type="button" class="btn btn-secondary spc-step-btn" onclick="ecmSpcIdleStep('${funcDef.key}',1)" aria-label="increase">+</button>
          </div></label></div>
        <div class="spc-btns">
        <button class="btn btn-secondary btn-xs" onclick="ecmSpcRead('${funcDef.key}')"><span>Read</span></button>
        <button class="btn btn-primary btn-xs" onclick="ecmSpcSetIdle('${funcDef.key}')"><span>Set idle speed</span></button></div>
        <div class="spc-hint">Range ${funcDef.min}&ndash;${funcDef.max} rpm in ${funcDef.step} rpm steps; a typed value is rounded to the nearest step before writing. Engine must be running and at operating temperature.</div>`;
    } else if (funcDef.type === 3) {
      body = `<div class="spc-btns"><button class="btn btn-primary btn-xs" onclick="ecmSpcRunConfirm('${funcDef.key}','Reset air-fuel values? Learned fuel trims are cleared and relearned while riding.')"><span>Run (${funcDef.delay} s)</span></button></div>`;
    } else if (funcDef.type === 4) {
      body = `<div class="spc-hint">Engine off, ignition on. After the sequence, attempt a normal start once.</div>
        <div class="spc-btns"><button class="btn btn-primary btn-xs" onclick="ecmSpcRunConfirm('${funcDef.key}','Start the security module pairing (password learn)?')"><span>Start pairing</span></button></div>`;
    } else if (funcDef.type === 9) {
      const procId = funcDef.proc || funcDef.key;
      body = `<div class="spc-hint">Each step appears in a pop-up with on-screen instructions. Keep the bike on a stand, engine off, ignition on.</div>
        <div class="spc-btns"><button class="btn btn-primary btn-xs" id="proc-start-${funcDef.key}" onclick="procStart('${procId}','${funcDef.key}')"><span>Start</span></button></div>`;
    } else if (funcDef.type === 5) {
      body = `<div class="spc-state" id="${id}-val">calibration value: not read</div>
        <div class="spc-fields">
          <label>Speedometer<input type="number" inputmode="numeric" class="input-dark" id="${id}-sp" min="30" max="130" oninput="ecmSpcSpeedoCalc('${funcDef.key}')"></label>
          <label>GPS<input type="number" inputmode="numeric" class="input-dark" id="${id}-gps" min="30" max="130" oninput="ecmSpcSpeedoCalc('${funcDef.key}')"></label>
          <label>New value<input type="number" inputmode="numeric" class="input-dark" id="${id}-in" min="${funcDef.min}" max="${funcDef.max}"></label>
        </div>
        <div class="spc-btns">
          <button class="btn btn-secondary btn-xs" onclick="ecmSpcRead('${funcDef.key}')"><span>Read</span></button>
          <button class="btn btn-primary btn-xs" onclick="ecmSpcRun('${funcDef.key}',document.getElementById('${id}-in').value)"><span>Write</span></button>
        </div>
        <div class="spc-hint">Suggested value = current &times; GPS &divide; speedometer (an estimate &mdash; the database has no formula). Range ${funcDef.min}&ndash;${funcDef.max}. Check the speedometer against GPS afterwards.</div>`;
    } else {
      body = '<div class="spc-hint">Not supported yet.</div>';
    }
    return `<div class="actuator-row-card spc-fn" id="${id}">
      <button type="button" class="spc-head" onclick="ecmSpcToggle('${funcDef.key}',${funcDef.type})"><span><strong>${spcEsc(funcDef.name)}</strong></span><span class="spc-caret">&#9656;</span></button>
      <div class="actuator-controls" id="${id}-body" style="display:none">
        <div class="spc-hint">${spcEsc(funcDef.desc)}</div>${funcDef.note ? `<div class="spc-hint">${spcEsc(funcDef.note)}</div>` : ''}
        ${body}</div></div>`;
  }).join('') + rawHtmlEnd;
}

// Collapsed-by-default special functions: entering a module no longer reads every value (too slow on a
// long list). Each function is a button; opening it reads only that function's current state, once.
function ecmSpcToggle(key, type) {
  const card = document.getElementById('spc-' + key);
  const body = document.getElementById('spc-' + key + '-body');
  if (!card || !body) return;
  const isHidden = getComputedStyle(body).display === 'none';
  body.style.display = isHidden ? 'block' : 'none';
  card.classList.toggle('spc-open', isHidden);
  if (isHidden && !card.dataset.read) { card.dataset.read = '1'; ecmSpcAutoRead(key, type); }
}

// Read the current state for the just-opened function (readable types only; actions/procedures don't read).
function ecmSpcAutoRead(key, type) {
  if (type === 99) { ecmSpcReadBoost(); return; }        // BCM Anti-theft PIN card
  if (type === 98) { fobStatusRead(); return; }          // BCM key-FOB status card
  if (type === 96) { fob1Read(); return; }               // BCM key-FOB assignment (1 FOB)
  if (type === 95) { fob2Read(); return; }               // BCM key-FOB assignment (2 FOBs)
  if (type === 97) { vehicleInfoRead(); return; }        // SPDO odometer & VIN card
  if (type === 94) { vinCardRead('ecm-vin'); return; }   // ECM VIN write card
  if (type === 91) { spdoIdRead(); return; }             // SPDO identity blocks (0157/0158)
  if (type === 90) { vinCardRead('spdo-vin'); return; }  // SPDO VIN write card
  if (type === 89) { vinCardRead('abs-vin'); return; }   // ABS VIN write card
  if (type === 88) { vinCardRead('rad-vin'); return; }   // RAD VIN write card
  if (type === 8) { ecmSpcTpmsConfigRead(key); return; } // TPMS status
  if (type === 7) { ecmSpcTpmsSensorRead(key); return; } // TPMS sensor IDs
  if (type === 1 || type === 2 || type === 5 || type === 6) ecmSpcRead(key, true); // idle / on-off / speedo-cal / option
  // types 3, 4, 9 are one-shot actions / guided procedures - nothing to pre-read
}

function ecmSpcTpmsConfigRead(key) {
  ecmSpcPost('tpms11', '').then(result => {
    if (!result) return;
    spcMsg(result.msg, result.ok);        // inline status line only - no popup on Read (result goes in the value line)
    const statusValueEl = document.getElementById('spc-' + key + '-val');
    if (!statusValueEl) return;
    statusValueEl.innerHTML = result.ok
      ? `<span class="spc-ok">OK</span> &mdash; raw status byte: 0x${(result.byte || 0).toString(16).toUpperCase().padStart(2, '0')}`
      : `<span class="spc-err">Error</span> &mdash; ${spcEsc(result.msg || 'read failed')}`;
  });
}

function ecmSpcTpmsSensorRead(key) {
  ecmSpcPost('tpms12/read', '').then(result => {
    if (!result) return;
    spcMsg(result.msg, result.ok);        // inline status line only - no popup on Read (result goes in the value line)
    const statusValueEl = document.getElementById('spc-' + key + '-val');
    if (!statusValueEl) return;
    if (result.ok) {
      statusValueEl.innerHTML = `<span class="spc-ok">OK</span> &mdash; front: ${spcEsc(result.front)}, rear: ${spcEsc(result.rear)}`;
      const frontInputEl = document.getElementById(id2(key, 'front')); if (frontInputEl && !frontInputEl.value) frontInputEl.value = result.front;
      const rearInputEl = document.getElementById(id2(key, 'rear')); if (rearInputEl && !rearInputEl.value) rearInputEl.value = result.rear;
    } else {
      statusValueEl.innerHTML = `<span class="spc-err">Error</span> &mdash; ${spcEsc(result.msg || 'read failed')}`;
    }
  });
}

function id2(key, suffix) { return 'spc-' + key + '-' + suffix; }

async function ecmSpcTpmsSensorWriteConfirm(key) {
  if (!(await hdConfirm('Do you really want to assign Front and Rear Sensor ID?', 'Assign Sensor ID'))) return;
  const front = (document.getElementById(id2(key, 'front')) || {}).value || '';
  const rear = (document.getElementById(id2(key, 'rear')) || {}).value || '';
  ecmSpcPost('tpms12/write', '', undefined, `&front=${encodeURIComponent(front)}&rear=${encodeURIComponent(rear)}&confirm=1`).then(result => {
    if (!result) return;
    spcMsg(result.msg, result.ok);
    hdAlert(result.msg, result.ok);
    const statusValueEl = document.getElementById('spc-' + key + '-val');
    if (statusValueEl) statusValueEl.innerHTML = result.ok ? '<span class="spc-ok">OK</span> &mdash; written' : `<span class="spc-err">Error</span> &mdash; ${spcEsc(result.msg || 'write failed')}`;
  });
}

// PIN read/change (BCM DID 0141, shown under the "Anti-theft PIN" card label - boostLabel()). The PIN is
// 5 digits, EACH DIGIT 1-9 (0 is never a valid PIN digit, so it is stripped live here, rejected with
// "Input number is out of range, valid range is 1 - 9"). Read shows the current PIN as
// the session's "Original" (pinOrig, captured once, same pattern as fobOrig below) so Apply's confirmation
// and the final success message can show "PIN <old> Change to <new>".
let pinOrig = null;
function pinValueInput(el) {
  const before = el.value;
  const hadInvalid = /[^1-9]/.test(before);   // a 0, or any non-digit, was present before stripping
  let v = before.replace(/[^1-9]/g, '');
  if (v.length > 5) v = v.slice(0, 5);
  el.value = v;
  if (hadInvalid) {
    // The invalid character (0 is never a valid PIN digit) was silently dropped above - tell the operator
    // why, instead of leaving them wondering why nothing appeared.
    spcMsg('Input number is out of range, valid range is 1 - 9', false);
  }
}

function ecmSpcBoostShow(result) {
  const statusEl = document.getElementById('spc-boost-status');
  const valueEl = document.getElementById('spc-boost-val');
  if (!statusEl) return;
  if (result.ok) {
    if (pinOrig === null) pinOrig = result.value || '';   // session-original, captured once (like fobOrig)
    statusEl.textContent = `Current (Original): ${result.value || '--'}`;
    // Same as every other special-function Read: prefill "New value" with what was just read, so it's
    // obvious what's there and the operator can edit from it instead of starting blank.
    if (valueEl) valueEl.value = result.value || '';
  } else {
    statusEl.innerHTML = `Current (Original): <span class="spc-err">${spcEsc(result.msg || 'read failed')}</span>`;
    if (valueEl) valueEl.value = '';
  }
}

function ecmSpcReadBoost() {
  // No popup on Read: the value (or the error) is shown in the Current line by ecmSpcBoostShow.
  const statusEl = document.getElementById('spc-boost-status');
  const valueEl = document.getElementById('spc-boost-val');
  if (statusEl) statusEl.textContent = 'Current (Original): reading...';
  if (valueEl) valueEl.value = '';
  ecmSpcPost('boost', '').then(result => { if (!result) return; spcMsg(result.msg, result.ok); ecmSpcBoostShow(result); });
}

async function ecmSpcApplyBoost() {
  const valueEl = document.getElementById('spc-boost-val');
  const digits = valueEl ? valueEl.value.trim() : '';
  // Each digit must be 1-9 (never 0) - caught here before anything reaches the bus.
  if (digits.length !== 5 || /0/.test(digits) || !/^[1-9]{5}$/.test(digits)) {
    spcMsg('Input number is out of range, valid range is 1 - 9', false); return;
  }
  const oldPin = pinOrig !== null ? pinOrig : '(not read yet)';
  if (!(await hdConfirm(`${boostLabel()} ${oldPin} Change to ${digits}?`, 'Type PIN'))) return;
  hdWait(`Writing the new PIN to the BCM (${boostLabel()})...`, 'Please wait');
  const result = await ecmSpcPost('boost/apply', '', undefined, `&value=${encodeURIComponent(digits)}`);
  hdWaitClose();
  if (!result) return;
  spcMsg(result.msg, result.ok);
  await hdAlert(result.ok ? `${boostLabel()} was Change to ${digits}` : result.msg, result.ok);
  ecmSpcBoostShow(result);
}

async function ecmSpcRestoreBoost() {
  if (!(await hdConfirm(`Write the original ${boostLabel()} value (saved before the first change in this session) back?`, 'Restore Original Value'))) return;
  hdWait(`Restoring the original ${boostLabel()} value...`, 'Please wait');
  const result = await ecmSpcPost('boost/restore', '');
  hdWaitClose();
  if (!result) return;
  spcMsg(result.msg, result.ok);
  await hdAlert(result.msg, result.ok);
  ecmSpcBoostShow(result);
}

function ecmSpcBusy(busyState) {
  ecmSpc.busy = busyState;
  document.querySelectorAll('#ecmspc-list button').forEach(buttonElement => { buttonElement.disabled = busyState; });
}

function ecmSpcPost(path, key, value, extra) {
  if (ecmSpc.busy) return Promise.resolve(null);
  ecmSpcBusy(true);
  spcMsg(`Working... (talking to the ${spcModuleName()})`, true);
  let url = `${ecmSpc.base}/${path}?key=${encodeURIComponent(key)}`;
  if (value !== undefined && value !== null) url += `&value=${encodeURIComponent(value)}`;
  if (extra) url += extra;
  return fetch(url, { method: 'POST', cache: 'no-store' })
    .then(response => response.json())
    .catch(() => { spcMsg('No response from the ESP32', false); return null; })
    .then(result => { ecmSpcBusy(false); return result; });
}

function ecmSpcShow(key, result) {
  // Every call here comes from an explicit read (auto-read-on-open, the per-function Read button, or the
  // refresh-read right after a successful Apply/Restore) - never from passive/background polling - so the
  // "New value" selector or input is always re-synced to what was actually just read, even if the user had
  // already changed it or read it before. 'touched' is still set afterwards so other code (e.g. ecmSpcApply)
  // can tell the field has a real value, but it no longer gates whether a fresh read is applied here.
  const element = document.getElementById('spc-' + key + '-val');
  if (!element || !result || !result.ok) return;
  ecmSpc.cur[key] = result;
  if (result.text !== undefined) {
    element.textContent = 'Current: ' + result.text + (result.changed ? '  (changed in this session)' : '');
    const selectElement = document.getElementById('spc-' + key + '-sel');
    if (selectElement) { selectElement.value = String(result.option); selectElement.dataset.touched = '1'; }
  } else if (result.enabled !== undefined) element.textContent = 'state: ' + (result.enabled ? 'ENABLED' : 'disabled');
  else if (result.idle !== undefined) {
    // Idle speed itself goes in the exported ".spc-state" line; the live engine rpm/coolant temp alongside it
    // are context for setting the idle speed (is the engine running yet, is it warmed up), not a saved value
    // of this function - they get their own non-exported line (see spcCardValues(), which only reads .spc-state).
    element.textContent = `idle: ${result.idle} rpm`;
    const liveElement = document.getElementById('spc-' + key + '-live');
    if (liveElement) liveElement.textContent = `Live: engine ${result.rpm} rpm, coolant ${result.temp} °C`;
    const inputElement = document.getElementById('spc-' + key + '-in');
    if (inputElement) { inputElement.value = result.idle; inputElement.dataset.touched = '1'; }
  } else if (result.value !== undefined) {
    element.textContent = 'calibration value: ' + result.value;
    const inputElement = document.getElementById('spc-' + key + '-in');
    if (inputElement) { inputElement.value = result.value; inputElement.dataset.touched = '1'; }
  }
}

// Report an operation result: inline banner + an acknowledge popup. `silent` skips the popup, used for the
// follow-up refresh-read after a run/restore so the user only gets one popup per action they took.
function ecmSpcReport(result, silent) {
  spcMsg(result.msg, result.ok);
  if (!silent) hdAlert(result.msg, result.ok);
}

// ----------------------------------------------------------------------------
// Auto read-all on entry: when the special-functions page opens, read every function that has a current
// value and fill it in, so the user sees the live state without clicking Read on each. While it runs, all
// controls are disabled, each pending function shows a "reading" spinner in its value line, and a read error
// is shown there (in red) instead of a popup. Runs sequentially - each read opens and closes its own session.
// ----------------------------------------------------------------------------
let ecmSpcAutoReading = false;
const SPC_READABLE_TYPES = [1, 2, 5, 6, 7, 8];   // function types that have a current value to read

function spcStateEl(rowKey) {
  return document.getElementById('spc-' + rowKey + '-val') || document.getElementById('spc-' + rowKey + '-status');
}
function spcSetReading(rowKey) {
  const el = spcStateEl(rowKey);
  if (el) { el.className = 'spc-state'; el.innerHTML = '<span class="spc-reading">reading</span>'; }
}
function spcSetError(rowKey, msg) {
  const el = spcStateEl(rowKey);
  if (el) { el.className = 'spc-state'; el.innerHTML = `<span class="spc-err">${spcEsc(msg || 'read failed')}</span>`; }
}
function ecmSpcSetListDisabled(disabled) {
  document.querySelectorAll('#ecmspc-list button, #ecmspc-list input, #ecmspc-list select').forEach(el => { el.disabled = disabled; });
}
// Fetch without the global busy toggle or banner/popup - used only by the auto-read loop.
function ecmSpcPostSilent(path, key, extra) {
  let url = `${ecmSpc.base}/${path}?key=${encodeURIComponent(key || '')}`;
  if (extra) url += extra;
  return fetch(url, { method: 'POST', cache: 'no-store' }).then(response => response.json()).catch(() => null);
}

async function ecmSpcReadOneAuto(funcDef) {
  const key = funcDef.key;
  let result;
  if (funcDef.type === 8) result = await ecmSpcPostSilent('tpms11', '');
  else if (funcDef.type === 7) result = await ecmSpcPostSilent('tpms12/read', '');
  else result = await ecmSpcPostSilent('read', key);
  if (!result) { spcSetError(key, 'no response from the ESP32'); return; }
  if (!result.ok) { spcSetError(key, result.msg); return; }
  if (funcDef.type === 8) {
    const el = spcStateEl(key);
    if (el) el.innerHTML = `<span class="spc-ok">OK</span> &mdash; raw status byte: 0x${(result.byte || 0).toString(16).toUpperCase().padStart(2, '0')}`;
  } else if (funcDef.type === 7) {
    const el = spcStateEl(key);
    if (el) el.innerHTML = `<span class="spc-ok">OK</span> &mdash; front: ${spcEsc(result.front)}, rear: ${spcEsc(result.rear)}`;
    const frontInputEl = document.getElementById(id2(key, 'front')); if (frontInputEl && !frontInputEl.value) frontInputEl.value = result.front;
    const rearInputEl = document.getElementById(id2(key, 'rear')); if (rearInputEl && !rearInputEl.value) rearInputEl.value = result.rear;
  } else {
    ecmSpcShow(key, result);
  }
}

async function ecmSpcReadBoostAuto() {
  const result = await ecmSpcPostSilent('boost', '');
  if (!result) { spcSetError('boost', 'no response from the ESP32'); return; }
  if (!result.ok) { spcSetError('boost', result.msg); return; }
  ecmSpcBoostShow(result);
}

async function ecmSpcReadAll() {
  const fns = (ecmSpc.cat && ecmSpc.cat.functions) || [];
  const readable = fns.filter(f => SPC_READABLE_TYPES.indexOf(f.type) !== -1);
  const hasBoost = spcIsBcm() && document.getElementById('spc-boost');
  const total = readable.length + (hasBoost ? 1 : 0);
  if (total === 0) return;

  ecmSpcAutoReading = true;
  ecmSpcSetListDisabled(true);
  readable.forEach(f => spcSetReading(f.key));
  if (hasBoost) spcSetReading('boost');

  let done = 0;
  for (const f of readable) {
    if (!ecmSpc.open) { ecmSpcAutoReading = false; return; }   // user left the page - stop
    spcMsg(`Reading current values... (${++done}/${total})`, true);
    await ecmSpcReadOneAuto(f);
  }
  if (hasBoost && ecmSpc.open) {
    spcMsg(`Reading current values... (${++done}/${total})`, true);
    await ecmSpcReadBoostAuto();
  }

  ecmSpcAutoReading = false;
  if (!ecmSpc.open) return;
  ecmSpcSetListDisabled(false);
  spcMsg('Current values loaded. Read, change or restore any function.', true);
}

// ----------------------------------------------------------------------------
// "Read all" / "Save all" (every special-functions menu). Read all opens every card and reads every current
// value (catalog types + Boost via ecmSpcReadAll, plus the custom BCM/SPDO cards). Save all - enabled only
// after a successful Read all - downloads a .txt with the module's ECU identification header followed by each
// function's name and its current value. The ECU header is read from the bike once per session and cached.
// ----------------------------------------------------------------------------
let spcReadAllDone = false;
let spcEcuInfoCache = {};

function spcModIdHex() { return spcIsBcm() ? '0x7E2' : (spcIsAbs() ? '0x7E1' : (spcIsSpdo() ? '0x7F2' : (spcIsRad() ? '0x7F1' : '0x7E0'))); }

async function spcReadAll() {
  const list = document.getElementById('ecmspc-list');
  if (!list) return;
  const readBtn = document.getElementById('spc-readall'), saveBtn = document.getElementById('spc-saveall');
  spcReadAllDone = false;
  if (saveBtn) saveBtn.disabled = true;
  if (readBtn) readBtn.disabled = true;
  // open every card so values are visible as they fill
  list.querySelectorAll('.spc-fn').forEach(card => {
    const body = card.querySelector('.actuator-controls');
    if (body) body.style.display = 'block';
    card.classList.add('spc-open');
    card.dataset.read = '1';
  });
  // 1) catalog readable types + Boost (own spinners + inline errors, no popups)
  await ecmSpcReadAll();
  if (!ecmSpc.open) return;
  // 2) custom cards that are not part of the module catalog
  if (spcIsBcm()) {
    // One BCM read serves the FOB-status card and both assignment cards (fobReadAll), instead of reading
    // the same serials three separate times during Read all.
    if (document.getElementById('spc-fob1') || document.getElementById('spc-fob2') || document.getElementById('spc-fobread')) {
      spcMsg('Reading key FOB assignment...', true);
      await fobReadAll();
    }
  } else if (spcIsSpdo()) {
    if (document.getElementById('spc-vehinfo')) { spcMsg('Reading odometer & VIN...', true); await vehicleInfoRead(); }
    if (document.getElementById('spc-spdo-vin')) { spcMsg('Reading VIN...', true); await vinCardRead('spdo-vin'); }
    if (document.getElementById('spc-spdo-idblocks')) { spcMsg('Reading identity blocks...', true); await spdoIdRead(); }
  } else if (spcIsRad()) {
    if (document.getElementById('spc-rad-vin')) { spcMsg('Reading VIN...', true); await vinCardRead('rad-vin'); }
  } else if (spcIsEcm()) {
    if (document.getElementById('spc-ecm-vin')) { spcMsg('Reading VIN...', true); await vinCardRead('ecm-vin'); }
  }
  spcReadAllDone = true;
  if (readBtn) readBtn.disabled = false;
  if (saveBtn) saveBtn.disabled = false;
  spcMsg('All current values read. You can now Save all to a text file.', true);
}

// Read the module's ECU identification (same source as the "Read ECU information" button). Returns {items:[{label,value}]}.
async function spcFetchEcuInfo(modId) {
  const out = { items: [] };
  try {
    const planResp = await fetch('/api/ecu/info/plan?id=' + encodeURIComponent(modId), { method: 'POST', cache: 'no-store' });
    let plan = (planResp.status !== 404) ? await planResp.json() : null;
    if (plan && plan.ok && plan.items) {
      for (let i = 0; i < plan.items.length; i++) {
        let value = 'N/A';
        for (let a = 0; a < 2; a++) {
          try { const r = await fetch('/api/ecu/info/item?id=' + encodeURIComponent(modId) + '&i=' + i, { method: 'POST', cache: 'no-store' }); const d = await r.json(); if (d && d.ok) { value = d.value || 'N/A'; break; } } catch (e) { /* retry */ }
          await sleepMs(80);
        }
        out.items.push({ label: plan.items[i], value });
      }
      return out;
    }
  } catch (e) { /* fall back */ }
  const rows = [['VIN', '0xF1EF'], ['Parts Number', '0xF1EA'], ['Calibration ID', '0xF1ED'], ['Homologation ID', '0xF1EE']];
  for (const row of rows) { const d = await fetchSingleDidWithRetry(modId, row[1]); out.items.push({ label: row[0], value: (d && d.value) || 'N/A' }); }
  return out;
}

// Align the value column: pad the label with dots to a fixed width so every "Current: ..." lines up. A label
// at/over the column just gets two spaces (rare long names like "Power lock switch (saddle bags / trunk)").
const SPC_LABEL_COL = 42;
function spcPadLabel(s) {
  s = String(s);
  if (s.length >= SPC_LABEL_COL) return s + '  ';
  return s + ' ' + '.'.repeat(SPC_LABEL_COL - s.length - 1) + ' ';
}
// Transient / placeholder state text that should never be saved.
const SPC_SKIP_STATE = /^(reading(\.{0,3}|…)?|not read|not assigned|—|–|-|\(none\))$/i;
// Pull the real value line(s) out of a card: turn <br> into separate lines, drop tags, collapse whitespace,
// and drop spinner/placeholder tokens (e.g. a leftover "reading").
function spcCardValues(card) {
  const out = [];
  card.querySelectorAll('.spc-state').forEach(s => {
    const txt = spcAsciiSafe(s.innerHTML.replace(/<br\s*\/?>/gi, '\n').replace(/<[^>]+>/g, '')
      .replace(/&amp;/g, '&').replace(/&mdash;/g, '-').replace(/&middot;/g, '-').replace(/&rarr;/g, '->'));
    txt.split('\n').forEach(line => {
      line = line.trim().replace(/\s+/g, ' ');
      if (line && !SPC_SKIP_STATE.test(line)) out.push(line);
    });
  });
  return out;
}
// Append "label .... value" for a short single value, or a title line + indented values for long/multi.
// A one-shot action (TPS reset, Clear adaptations, a guided procedure, ...) has nothing to read - it is left
// out of the file entirely rather than printed with a placeholder, since there is no value to save.
function spcEmitKV(lines, title, parts) {
  if (parts.length === 0) return;
  if (parts.length === 1 && title.length < SPC_LABEL_COL) { lines.push(spcPadLabel(title) + parts[0]); return; }
  lines.push(title + ':');
  parts.forEach(p => lines.push('    ' + p));
}
// Replace characters that some plain-text viewers render as boxes/question marks with ASCII equivalents.
function spcAsciiSafe(s) { return String(s).replace(/[‒-―−]/g, '-').replace(/°/g, 'deg').replace(/[‘’]/g, "'").replace(/[“”]/g, '"'); }
function spcStamp() { const d = new Date(), p = n => String(n).padStart(2, '0'); return d.getFullYear() + p(d.getMonth() + 1) + p(d.getDate()) + '-' + p(d.getHours()) + p(d.getMinutes()); }
function spcDownloadText(name, text) {
  const blob = new Blob([text], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a'); a.href = url; a.download = name; document.body.appendChild(a); a.click();
  setTimeout(() => { URL.revokeObjectURL(url); a.remove(); }, 500);
}

async function spcSaveAll() {
  if (!spcReadAllDone) { spcMsg('Read all values first (press Read all).', false); return; }
  const modName = spcModuleName(), modId = spcModIdHex();
  const saveBtn = document.getElementById('spc-saveall'), lbl = saveBtn && saveBtn.querySelector('span');
  if (saveBtn) saveBtn.disabled = true;
  if (lbl) lbl.textContent = 'Saving...';
  let ecu = spcEcuInfoCache[modId];
  if (!ecu) { spcMsg('Reading ECU information for the file header...', true); ecu = await spcFetchEcuInfo(modId); spcEcuInfoCache[modId] = ecu; }

  const lines = [];
  lines.push('HD diagnostic tool - ' + modName + ' special functions');
  lines.push('Module       : ' + modName + ' (' + modId + ')');
  lines.push('Saved        : ' + new Date().toLocaleString());
  lines.push('');
  lines.push('== ECU identification ==');
  if (ecu && ecu.items && ecu.items.length) ecu.items.forEach(it => lines.push(spcPadLabel(it.label) + spcAsciiSafe(it.value)));
  else lines.push('(ECU identification not available)');
  lines.push('');
  lines.push('== Special functions - current values ==');
  document.querySelectorAll('#ecmspc-list .spc-fn').forEach(card => {
    const head = card.querySelector('.spc-head strong');
    const title = head ? head.textContent.trim() : '(function)';
    const id = card.id || '';
    // The Key FOB assignment cards are programming tools - their "current" just repeats Key FOB status, so
    // don't dump the serials again here (consolidate: the serials live under "Key FOB status" only).
    if (id === 'spc-fob1' || id === 'spc-fob2') { lines.push(spcPadLabel(title) + '(programming tool)'); return; }
    spcEmitKV(lines, title, spcCardValues(card));
  });
  const text = lines.join('\r\n') + '\r\n';
  const fname = 'hd-' + modName.toLowerCase() + '-special-' + spcStamp() + '.txt';
  spcDownloadText(fname, text);
  if (saveBtn) saveBtn.disabled = false;
  if (lbl) lbl.textContent = 'Save all';
  spcMsg('Saved ' + fname, true);
}

function ecmSpcRead(key) {
  // Read never pops up (per operator request): the value or the error goes into the "Current:" line.
  ecmSpcPost('read', key).then(result => {
    if (!result) return;
    spcMsg(result.msg, result.ok);
    if (result.ok) {
      // An explicit Read click always syncs the "New value" selector to what was actually read, even if the
      // user had already changed it - clearing 'touched' first lets ecmSpcShow() re-apply result.option.
      const selectElement = document.getElementById('spc-' + key + '-sel');
      if (selectElement) selectElement.dataset.touched = '';
      const inputElement = document.getElementById('spc-' + key + '-in');
      if (inputElement) inputElement.dataset.touched = '';
      ecmSpcShow(key, result);
    }
    else { const el = document.getElementById('spc-' + key + '-val'); if (el) el.innerHTML = `<span class="spc-err">${spcEsc(result.msg || 'read failed')}</span>`; }
  });
}

function ecmSpcRun(key, value) {
  ecmSpcPost('run', key, value).then(result => {
    if (!result) return;
    ecmSpcReport(result);
    if (result.ok) {
      const funcDef = (ecmSpc.cat.functions || []).find(funcEntry => funcEntry.key === key);
      if (funcDef && (funcDef.type === 2 || funcDef.type === 1 || funcDef.type === 5)) ecmSpcRead(key, true);
      if (funcDef && funcDef.type === 3) ecmSpcWatch();
    }
  });
}

async function ecmSpcApply(key) {
  const funcDef = (ecmSpc.cat.functions || []).find(funcEntry => funcEntry.key === key);
  const selectElement = document.getElementById('spc-' + key + '-sel');
  if (!funcDef || !selectElement) return;
  const label = (funcDef.opts || [])[parseInt(selectElement.value, 10)] || '';
  const confirmMsg = `Set to: ${label}${funcDef.warn ? '\n\n' + funcDef.warn : ''}\n\nWrite this value to the ${spcModuleName()}?`;
  if (!(await hdConfirm(confirmMsg, funcDef.name))) return;
  ecmSpcPost('run', key, selectElement.value, funcDef.warn ? '&confirm=1' : '').then(result => {
    if (!result) return;
    ecmSpcReport(result);
    if (result.ok) { selectElement.dataset.touched = ''; ecmSpcShow(key, result); }
  });
}

async function ecmSpcRunConfirm(key, text) {
  if (!(await hdConfirm(text, 'Confirm'))) return;
  ecmSpcPost('run', key, 1, '&confirm=1').then(result => {
    if (!result) return;
    const funcDef = (ecmSpc.cat.functions || []).find(funcEntry => funcEntry.key === key);
    // For a timed watch (type 3) the real result comes when the watch finishes, so don't popup now.
    ecmSpcReport(result, result.ok && funcDef && funcDef.type === 3);
    if (result.ok && funcDef && funcDef.type === 3) ecmSpcWatch();
  });
}

// ---- ECM standalone actions (TPS reset, Clear adaptations, VIN write) - plain Run / Read+Write, each with
// its own confirmation popup, same as every other one-shot action here. ----
async function ecmTpsResetRun() {
  if (!(await hdConfirm('Reset the throttle-position (TPS) adaptation and relearn idle?\n\nEngine off, ignition on.', 'TPS reset / throttle relearn'))) return;
  ecmSpcPost('tps_reset', '').then(result => { if (!result) return; ecmSpcReport(result); });
}

async function ecmClearAdaptRun() {
  if (!(await hdConfirm('Clear ECM diagnostic info and adaptations, then relearn on next start?', 'Clear adaptations + relearn'))) return;
  ecmSpcPost('relearn', '').then(result => { if (!result) return; ecmSpcReport(result); });
}

// Generic VIN read/write for every module's VIN card (ECM, ABS, SPDO, RAD - all route through
// /api/<module>/special/vin/{read,write} via ecmSpcPost(), which already targets the open module's base
// path). `prefix` is the card's DOM-id prefix ('ecm-vin', 'abs-vin', 'spdo-vin', 'rad-vin').
function vinCardRead(prefix) {
  // Read never pops up (per operator request, same as every other Read here): the VIN or the error goes
  // into the "Current VIN:" line.
  return ecmSpcPost('vin/read', '').then(result => {
    if (!result) return;
    spcMsg(result.msg, result.ok);
    const el = document.getElementById(prefix + '-cur');
    if (result.ok) {
      if (el) el.textContent = 'Current VIN: ' + (result.vin || '');
      // Always re-sync to what was just read, same as every other special-function Read (explicit reads
      // should never leave a stale value sitting in the field from before).
      const inputEl = document.getElementById(prefix + '-in');
      if (inputEl) inputEl.value = result.vin || '';
    } else if (el) el.innerHTML = `<span class="spc-err">${spcEsc(result.msg || 'read failed')}</span>`;
  });
}

async function vinCardWrite(prefix) {
  const inputEl = document.getElementById(prefix + '-in');
  const vin = ((inputEl && inputEl.value) || '').trim().toUpperCase();
  if (vin.length !== 17) { hdAlert('Enter exactly 17 characters for the VIN.', false); return; }
  if (!(await hdConfirm('Write this VIN to the ' + spcModuleName() + '?\n\n' + vin, 'VIN write (' + spcModuleName() + ')'))) return;
  ecmSpcPost('vin/write', '', undefined, `&vin=${encodeURIComponent(vin)}`).then(result => {
    if (!result) return;
    ecmSpcReport(result);
    if (result.ok) {
      const el = document.getElementById(prefix + '-cur');
      if (el) el.textContent = 'Current VIN: ' + (result.vin || vin);
      if (inputEl) inputEl.dataset.touched = '1';
    }
  });
}

// ---- SPDO identity blocks (DID 0157/0158) - read, client-side save/load, and write, so a replacement
// cluster can be paired from this one card without an external diagnostic tool or the old guided wizard. ----
let spdoIdBlocksLast = null;
function spdoIdRead() {
  const el157 = document.getElementById('spdo-id-157');
  const el158 = document.getElementById('spdo-id-158');
  const in157 = document.getElementById('spdo-id-157-in');
  const in158 = document.getElementById('spdo-id-158-in');
  const saveBtn = document.getElementById('spdo-id-save');
  if (el157) el157.textContent = 'Block 1 (DID 0157): reading...';
  if (el158) el158.textContent = 'Block 2 (DID 0158): reading...';
  return ecmSpcPost('idblocks', '').then(result => {
    if (!result) return;
    spcMsg(result.msg, result.ok);
    // Each block is reported on its own merits (full 12 bytes = 24 hex chars, or a shorter/partial read, or no
    // reply at all) - the firmware's overall "ok" only means BOTH came back complete, so showing just that one
    // flag here would hide which block (if either) actually failed.
    const blk157 = result.blk157 || '', blk158 = result.blk158 || '';
    const full157 = blk157.length === 24, full158 = blk158.length === 24;
    if (el157) el157.innerHTML = !blk157 ? `<span class="spc-err">Block 1 (DID 0157): ${spcEsc(result.msg || 'read failed')}</span>`
      : full157 ? 'Block 1 (DID 0157): <strong>' + spcEsc(blk157) + '</strong>'
      : `Block 1 (DID 0157): <span class="spc-err">incomplete - ${blk157.length / 2} of 12 bytes: ${spcEsc(blk157)}</span>`;
    if (el158) el158.innerHTML = !blk158 ? `<span class="spc-err">Block 2 (DID 0158): ${spcEsc(result.msg || 'read failed')}</span>`
      : full158 ? 'Block 2 (DID 0158): <strong>' + spcEsc(blk158) + '</strong>'
      : `Block 2 (DID 0158): <span class="spc-err">incomplete - ${blk158.length / 2} of 12 bytes: ${spcEsc(blk158)}</span>`;
    if (saveBtn) saveBtn.disabled = !(full157 && full158);
    // Prefill the editable fields with whatever was actually read, complete or not, same as the VIN cards
    // prefill their field - makes it obvious what's there and lets a short read still be corrected by hand.
    if (in157 && blk157) in157.value = blk157;
    if (in158 && blk158) in158.value = blk158;
    spdoIdBlocksLast = (full157 && full158) ? { blk157, blk158 } : null;
  });
}
// Save/Load share one plain-text format so a file written by Save can be read back by Load unchanged.
function spdoIdFileText(blk157, blk158) {
  return 'SPDO identity blocks (for speedometer pairing)\r\nDID 0157: ' + (blk157 || '') + '\r\nDID 0158: ' + (blk158 || '') + '\r\n';
}
function spdoIdSave() {
  if (!spdoIdBlocksLast) return;
  const text = spdoIdFileText(spdoIdBlocksLast.blk157, spdoIdBlocksLast.blk158);
  const blob = new Blob([text], { type: 'text/plain' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url; link.download = 'spdo_identity_blocks.txt';
  document.body.appendChild(link); link.click(); document.body.removeChild(link);
  URL.revokeObjectURL(url);
}
function spdoIdLoadFile(fileInput) {
  const file = fileInput && fileInput.files && fileInput.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = () => {
    const text = String(reader.result || '');
    const match157 = text.match(/DID\s*0157:\s*([0-9A-Fa-f]{24})/);
    const match158 = text.match(/DID\s*0158:\s*([0-9A-Fa-f]{24})/);
    if (!match157 || !match158) { hdAlert('That file is not in the expected format (expects the lines this card\'s Save button writes).', false); fileInput.value = ''; return; }
    const in157 = document.getElementById('spdo-id-157-in');
    const in158 = document.getElementById('spdo-id-158-in');
    if (in157) in157.value = match157[1].toUpperCase();
    if (in158) in158.value = match158[1].toUpperCase();
    spcMsg('Identity blocks loaded from ' + file.name + ' - check the fields, then Write.', true);
    fileInput.value = '';
  };
  reader.onerror = () => { hdAlert('Could not read that file.', false); fileInput.value = ''; };
  reader.readAsText(file);
}
async function spdoIdWrite() {
  const in157 = document.getElementById('spdo-id-157-in');
  const in158 = document.getElementById('spdo-id-158-in');
  const blk157 = ((in157 && in157.value) || '').trim().toUpperCase();
  const blk158 = ((in158 && in158.value) || '').trim().toUpperCase();
  if (!/^[0-9A-F]{24}$/.test(blk157) || !/^[0-9A-F]{24}$/.test(blk158)) { hdAlert('Enter 24 hex digits for both identity blocks (Read, Load, or paste them in first).', false); return; }
  if (!(await hdConfirm('Write these identity blocks to the ' + spcModuleName() + '?\n\nDID 0157: ' + blk157 + '\nDID 0158: ' + blk158, 'Identity blocks write (Speedometer)'))) return;
  ecmSpcPost('idblocks/write', '', undefined, `&blk157=${encodeURIComponent(blk157)}&blk158=${encodeURIComponent(blk158)}`).then(result => {
    if (!result) return;
    ecmSpcReport(result);
  });
}

// ---- Generic interactive guided-procedure runner (type 9, /api/proc/*) ----
// Shared by every module (ABS bleed, TPS reset, speedo pairing, FOB assignment, ...). The firmware
// returns a JSON status: prompt, whether it's a yes/no question or an operator input, live telemetry,
// and done/aborted. Only one procedure runs at a time; procActiveKey tracks which card is showing it.
let procActiveKey = null;
let procBusy = false;

function procPostRaw(path, qs) {
  if (procBusy) return Promise.resolve(null);
  procBusy = true;
  return fetch('/api/proc/' + path + (qs ? ('?' + qs) : ''), { method: 'POST', cache: 'no-store' })
    .then(r => r.json()).catch(() => null)
    .then(res => { procBusy = false; return res; });
}

function procEl(suffix) { return procActiveKey ? document.getElementById('proc-' + suffix + '-' + procActiveKey) : null; }

// ---- Popup-driven guided procedure (ABS bleed etc.) ------------------------
// Every info/question/result step is shown in the #modal-proc popup with the right buttons. All technical
// detail (commands, IDs, CAN addresses) stays on the USB serial console - the popup is mechanic-only.
let procTitle = 'Procedure';
let procStartKey = null;
let procLast = null;   // last step result, so an in-modal "Keep going" can restore the current step

function procModalResetStartBtn() {
  if (procStartKey) {
    const b = document.getElementById('proc-start-' + procStartKey);
    if (b) { b.disabled = false; const s = b.querySelector('span'); if (s) s.textContent = 'Start'; }
  }
  procStartKey = null; procActiveKey = null;
}

function procModalBusy(on) {
  const sp = document.getElementById('proc-modal-spinner');
  const btns = document.getElementById('proc-modal-btns');
  if (sp) sp.style.display = on ? 'flex' : 'none';
  if (btns) { btns.style.display = on ? 'none' : ''; }
}

// ---- Live master-cylinder pressure readout + press/release gate ------------
// While a popup waits, poll /api/proc/live a few times a second, show the pressure, and (when the step is
// flagged) keep "Continue" locked until the brake is actually pressed (>2000 kPa) or released (<1000 kPa).
const PROC_GATE_PRESS_KPA = 2000;      // master-cylinder pressure above this = brake applied
const PROC_GATE_RELEASE_KPA = 1000;    // below this = brake released
const PROC_GATE_CONFIRM_READS = 4;     // consecutive 500 ms reads needed before auto-advancing (~2 s)
// ABS pump bleed cycle (pumpwait) — TEST build: the module runs its own multi-pulse routine. We only cap the
// step for safety; the technician ends it by hand once the pump goes quiet. Everything is shown on screen.
const PROC_PUMP_MAX_MS = 180000;       // 3-minute safety cap (auto-ends the step if not ended manually)
const PROC_PUMP_OSC_KPA = 700;         // pressure swing over the recent window that counts as "pumping"
const PROC_PUMP_TARGET_PULSES = 3;     // ABS runs 3 pump pulses; Continue unlocks once all 3 are seen
let procLiveTimer = null, procGate = null, procGateCount = 0;
let procPumpStart = 0, procPumpSeenOn = false, procPumpLastOn = 0, procPressRing = [];
let procPumpPulses = 0, procPumpPrevOn = false;

function procFmtMMSS(ms) {
  const s = Math.max(0, Math.floor(ms / 1000));
  return String(Math.floor(s / 60)).padStart(2, '0') + ':' + String(s % 60).padStart(2, '0');
}

function procStopLive() {
  if (procLiveTimer) { clearInterval(procLiveTimer); procLiveTimer = null; }
  procGate = null; procGateCount = 0;
}

function procBusyMessage(fromPrompt) {
  const p = (fromPrompt || '').toLowerCase();
  if (p.indexOf('pump') >= 0)           return 'Starting the ABS pump…';
  if (p.indexOf('apply and hold') >= 0) return 'Bleeding the circuit — cycling the ABS valves. Keep holding the brake.';
  if (p.indexOf('firmness') >= 0)       return 'Reading brake pressure…';
  return 'Working…';
}

// Advance the procedure, showing an operation message (and the last pressure) while the tool works.
function procAdvance(answer, busyMsg) {
  procStopLive();
  const msgEl = document.getElementById('proc-modal-message');
  const telemEl = document.getElementById('proc-modal-telem');
  if (msgEl && busyMsg) msgEl.textContent = busyMsg;
  if (telemEl) telemEl.style.display = '';   // keep the last pressure visible under the operation message
  procModalBusy(true);
  procPostRaw('step', 'answer=' + answer).then(procRender);
}

function procLiveTick() {
  fetch('/api/proc/live', { cache: 'no-store' }).then(r => r.json()).then(d => {
    const telemEl = document.getElementById('proc-modal-telem');
    const kpa = (d && d.ok && typeof d.mc_kpa === 'number') ? d.mc_kpa : -1;
    const have = kpa >= 0;
    let line = have ? ('Brake pressure: ' + Math.round(kpa) + ' kPa') : 'Brake pressure: reading…';

    // ABS pump bleed cycle (TEST): monitor live and show everything on screen. End by hand; 3-min safety cap.
    if (procGate === 'pumpwait') {
      const now = Date.now();
      const pumpOn = !!(d && d.pump);
      if (procPumpPrevOn && !pumpOn) procPumpPulses++;   // counted one pump pulse (RUNNING -> idle)
      procPumpPrevOn = pumpOn;
      if (pumpOn) { procPumpSeenOn = true; procPumpLastOn = now; }
      procPressRing.push(have ? kpa : (procPressRing.length ? procPressRing[procPressRing.length - 1] : 0));
      if (procPressRing.length > 8) procPressRing.shift();
      const spread = procPressRing.length >= 2 ? (Math.max.apply(null, procPressRing) - Math.min.apply(null, procPressRing)) : 0;
      const pumping = spread > PROC_PUMP_OSC_KPA;
      const elapsed = now - procPumpStart;
      // Done when all 3 pulses are seen, OR (fallback) the pump has been idle longer than any inter-pulse pause.
      const complete = (procPumpPulses >= PROC_PUMP_TARGET_PULSES) || (procPumpSeenOn && (now - procPumpLastOn) > 25000);
      let pumpState;
      if (pumpOn) pumpState = 'RUNNING (pulse ' + Math.min(procPumpPulses + 1, PROC_PUMP_TARGET_PULSES) + ' of ' + PROC_PUMP_TARGET_PULSES + ')';
      else if (procPumpSeenOn) pumpState = 'idle — ' + procPumpPulses + ' of ' + PROC_PUMP_TARGET_PULSES + ' pulses';
      else pumpState = 'not reported';
      const statusLine = pumping
        ? '<span style="color:#22c55e;font-weight:600">✓ Pumping OK</span>'
        : '<span style="color:#ff9d2e;font-weight:600">⚠ Please keep pumping</span>';
      const doneLine = complete
        ? '<span style="color:#22c55e;font-weight:600">✓ Pump cycle complete — press Continue.</span>'
        : '<span style="opacity:0.75">Waiting for the ABS pump to finish its ' + PROC_PUMP_TARGET_PULSES + ' pulses…</span>';
      if (telemEl) {
        // Status centred right under the popup message, then a gap, then 3 detail lines.
        telemEl.innerHTML =
          '<div style="text-align:center">' + statusLine + '</div>' +
          '<div style="text-align:center;margin-top:2px">' + doneLine + '</div>' +
          '<div style="margin-top:14px">⏱ Elapsed time ' + procFmtMMSS(elapsed) + '</div>' +
          '<div>Brake pressure: ' + (have ? (Math.round(kpa) + ' kPa') : 'reading…') + '</div>' +
          '<div>ABS pump: ' + spcEsc(pumpState) + '</div>';
        telemEl.style.display = '';
      }
      const pbtn = document.getElementById('proc-pump-continue');   // enable by id (ref would be cleared by procStopLive)
      if (pbtn) { pbtn.disabled = !complete; pbtn.style.opacity = complete ? '1' : '0.5'; }
      if (elapsed > PROC_PUMP_MAX_MS) procAdvance(1, 'Stopping the ABS pump…');   // safety cap only
      return;
    }

    if (procGate === 'press' || procGate === 'release') {
      let sat = false, hint = '';
      if (procGate === 'press')   { sat = have && kpa > PROC_GATE_PRESS_KPA;   hint = sat ? 'holding…' : ('press the brake — needs over ' + PROC_GATE_PRESS_KPA + ' kPa'); }
      if (procGate === 'release') { sat = have && kpa < PROC_GATE_RELEASE_KPA; hint = sat ? 'released…' : ('release the brake — needs under ' + PROC_GATE_RELEASE_KPA + ' kPa'); }
      procGateCount = sat ? (procGateCount + 1) : 0;
      const pct = Math.min(100, Math.round(procGateCount / PROC_GATE_CONFIRM_READS * 100));
      if (telemEl) {
        telemEl.innerHTML = spcEsc(line + (have ? ('  —  ' + hint) : '')) +
          '<div style="margin-top:7px;height:9px;border-radius:5px;background:rgba(255,255,255,0.12);overflow:hidden">' +
          '<div style="height:100%;width:' + pct + '%;background:#ff8a00;transition:width 0.15s"></div></div>';
        telemEl.style.display = '';
      }
      if (procGateCount >= PROC_GATE_CONFIRM_READS) procAdvance(1, procBusyMessage(procLast && procLast.prompt));
      return;
    }
    if (telemEl) { telemEl.textContent = line; telemEl.style.display = ''; }
  }).catch(() => {});
}

function procStartLive(gate) {
  procStopLive();
  procGate = (gate === 'press' || gate === 'release' || gate === 'pumpwait') ? gate : null;
  procGateCount = 0;
  if (procGate === 'pumpwait') { procPumpStart = Date.now(); procPumpLastOn = Date.now(); procPumpSeenOn = false; procPressRing = []; procPumpPulses = 0; procPumpPrevOn = false; }
  procLiveTick();
  procLiveTimer = setInterval(procLiveTick, 500);
}

function procRender(result) {
  const modal = document.getElementById('modal-proc');
  const titleEl = document.getElementById('proc-modal-title');
  const msgEl = document.getElementById('proc-modal-message');
  const telemEl = document.getElementById('proc-modal-telem');
  const inputEl = document.getElementById('proc-modal-input');
  const btns = document.getElementById('proc-modal-btns');
  if (!modal || !btns) return;
  if (!result) { procModalBusy(false); return; }
  procStopLive();
  procModalBusy(false);
  procLast = result;
  const done = result.done || result.aborted;

  if (titleEl) { titleEl.textContent = procTitle; titleEl.style.color = done ? (result.aborted ? 'var(--red,#ef4444)' : 'var(--green,#22c55e)') : ''; }
  if (msgEl) msgEl.textContent = result.prompt || '';
  if (telemEl) { telemEl.textContent = ''; telemEl.style.display = 'none'; }   // the live poll fills this while waiting

  const isInput = (!done && result.kind === 'input');
  if (inputEl) { inputEl.style.display = isInput ? '' : 'none'; if (isInput) { inputEl.value = ''; inputEl.placeholder = result.field || ''; setTimeout(() => inputEl.focus(), 60); } }

  const addBtn = (label, cls, fn) => {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'btn ' + cls + ' flex-1';
    b.textContent = label; b.onclick = fn; btns.appendChild(b); return b;
  };
  btns.innerHTML = '';
  if (done) {
    addBtn('OK', 'btn-primary', () => { modal.classList.remove('show'); procModalResetStartBtn(); });
    return;
  }
  if (isInput) {
    addBtn('Cancel', 'btn-secondary', procAbort);
    addBtn('Submit', 'btn-primary', procSubmitInput);
  } else if (result.gate === 'pumpwait') {
    // Live monitor; Continue unlocks after the 3rd pump pulse (or the 3-min safety cap auto-advances).
    addBtn('Cancel', 'btn-danger', procAbort);
    const pbtn = addBtn('Continue', 'btn-primary', () => procAdvance(1, 'Stopping the ABS pump…'));
    pbtn.id = 'proc-pump-continue'; pbtn.disabled = true; pbtn.style.opacity = '0.5';
    // Developer test mode: skip the pump-cycle wait (Settings > Developer test).
    if (hdDevTestEnabled) addBtn('Continue (debug)', 'btn-secondary', () => procAdvance(1, 'Stopping the ABS pump…'));
  } else if (result.gate === 'press' || result.gate === 'release') {
    // No Continue — the step auto-advances once the brake is confirmed pressed/released.
    addBtn('Cancel', 'btn-danger', procAbort);
    // Developer test mode: skip the live brake-pressure check (Settings > Developer test).
    if (hdDevTestEnabled) addBtn('Continue (debug)', 'btn-secondary', () => procAdvance(1, procBusyMessage(result.prompt)));
  } else if (result.question) {
    addBtn('No', 'btn-secondary', () => procStep(0));
    addBtn('Yes', 'btn-primary', () => procStep(1));
  } else {
    addBtn('Cancel', 'btn-danger', procAbort);
    addBtn('Continue', 'btn-primary', () => procStep(1));
  }
  // Live pressure feed only where it's needed: the press/release gates and the pump-wait monitor.
  if (result.gate === 'press' || result.gate === 'release' || result.gate === 'pumpwait') procStartLive(result.gate);
}

// ABS bleed confirm: everything the operator needs before starting (bike-on-stand, the common safety
// checklist, and the "do a manual bleed first" reminder) in ONE popup, instead of three separate ones.
function absBleedConfirmText(procId) {
  const circuit = (procId || '').indexOf('rear') !== -1 ? 'rear' : 'front';
  return 'This runs a live ABS bleed on the ' + circuit + ' circuit.\n\n' +
    'Before you start:\n' +
    '• The bike must be on a stand, wheels off the ground, engine off, ignition on.\n' +
    '• A proper ABS brake bleed is required any time a hydraulic brake connection is loosened or ' +
    'replaced (master cylinders, ABS module, calipers, hydraulic brake switch, or any brake line).\n' +
    '• The battery must be fully charged, and the ignition must stay on for the whole procedure.\n' +
    '• Do not repeat any step without a 30 second cool-down.\n\n' +
    'Perform a manual hydraulic brake bleed on the ' + circuit + ' hydraulic system first, if you have not already.';
}

// Start-confirmation text for any guided procedure: the ABS bleed circuits get their full safety checklist
// (absBleedConfirmText); every other guided procedure (TPS reset, VIN write, speedo pairing, FOB assignment,
// ...) gets its own catalog description plus the generic on-stand/engine-off/ignition-on reminder, instead of
// the ABS-bleed text being shown for every procedure regardless of what it actually does.
function procConfirmText(procId, funcDef) {
  if ((procId || '').indexOf('abs_bleed') !== -1) return absBleedConfirmText(procId);
  const desc = (funcDef && funcDef.desc) ? funcDef.desc : 'Run this guided procedure?';
  return desc + '\n\nKeep the bike on a stand, engine off, ignition on.';
}

async function procStart(procId, key) {
  const funcDef = ((ecmSpc.cat && ecmSpc.cat.functions) || []).find(f => f.key === key);
  procTitle = funcDef ? funcDef.name : 'Procedure';
  if (!(await hdConfirm(procConfirmText(procId, funcDef), 'Start ' + procTitle))) return;
  procActiveKey = key; procStartKey = key;
  const startBtn = document.getElementById('proc-start-' + key);
  if (startBtn) { startBtn.disabled = true; const s = startBtn.querySelector('span'); if (s) s.textContent = 'Running…'; }
  const modal = document.getElementById('modal-proc');
  const msgEl = document.getElementById('proc-modal-message');
  if (msgEl) msgEl.textContent = 'Starting…';
  if (modal) modal.classList.add('show');
  procModalBusy(true);
  procPostRaw('start', 'id=' + encodeURIComponent(procId)).then(result => {
    if (!result) { if (modal) modal.classList.remove('show'); procModalResetStartBtn(); return; }
    procRender(result);
  });
}

function procStep(answer) { procStopLive(); procModalBusy(true); procPostRaw('step', 'answer=' + answer).then(procRender); }

function procSubmitInput() {
  const inputEl = document.getElementById('proc-modal-input');
  const v = inputEl ? (inputEl.value || '').trim().replace(/[\s-]/g, '') : '';
  if (!v) { if (inputEl) inputEl.focus(); return; }
  procStopLive();
  procModalBusy(true);
  procPostRaw('step', 'answer=1&input=' + encodeURIComponent(v)).then(procRender);
}

// Stop confirmation shown INSIDE the proc popup (avoids stacking a second modal behind it).
function procAbort() {
  procStopLive();
  const btns = document.getElementById('proc-modal-btns');
  const msgEl = document.getElementById('proc-modal-message');
  const telemEl = document.getElementById('proc-modal-telem');
  const inputEl = document.getElementById('proc-modal-input');
  if (!btns) return;
  if (telemEl) telemEl.style.display = 'none';
  if (inputEl) inputEl.style.display = 'none';
  if (msgEl) msgEl.textContent = 'Stop this procedure now? The ABS will be returned to normal operation.';
  btns.innerHTML = '';
  const add = (l, c, f) => { const b = document.createElement('button'); b.type = 'button'; b.className = 'btn ' + c + ' flex-1'; b.textContent = l; b.onclick = f; btns.appendChild(b); };
  add('Keep going', 'btn-secondary', () => procRender(procLast));
  add('Stop now', 'btn-danger', () => { procModalBusy(true); procPostRaw('abort', '').then(procRender); });
}

async function ecmSpcRestore(key) {
  if (!(await hdConfirm('Write the original value (saved before the first change in this session) back?', 'Restore Original Value'))) return;
  ecmSpcPost('restore', key).then(result => {
    if (!result) return;
    ecmSpcReport(result);
    if (result.ok) ecmSpcRead(key, true);
  });
}

function ecmSpcWatch() {
  if (ecmSpc.poll) clearInterval(ecmSpc.poll);
  ecmSpc.poll = setInterval(() => {
    fetch('/api/ecm/special/status', { cache: 'no-store' }).then(response => response.json()).then(statusResult => {
      spcMsg(statusResult.msg, true);
      if (!statusResult.busy) { clearInterval(ecmSpc.poll); ecmSpc.poll = null; hdAlert(statusResult.msg, true); }   // popup the final result when the timed function finishes
    }).catch(() => { clearInterval(ecmSpc.poll); ecmSpc.poll = null; });
  }, 1000);
}

function ecmSpcSpeedoCalc(key) {
  const getFieldValue = fieldId => parseFloat((document.getElementById('spc-' + key + '-' + fieldId) || {}).value);
  const speedoValue = getFieldValue('sp'), gpsValue = getFieldValue('gps');
  const currentCalibration = ecmSpc.cur[key] && ecmSpc.cur[key].value;
  const inputElement = document.getElementById('spc-' + key + '-in');
  if (inputElement && currentCalibration && speedoValue > 0 && gpsValue > 0) inputElement.value = Math.round(currentCalibration * gpsValue / speedoValue);
}

// Idle speed: snap a value to the nearest valid step (min + N*step, clamped), matching the firmware's own
// rounding so the confirmation shows exactly what will be written. The +/- buttons move by one step.
function ecmSpcIdleSnap(key, rawValue) {
  const funcDef = (ecmSpc.cat.functions || []).find(f => f.key === key);
  if (!funcDef) return Math.round(rawValue);
  const min = funcDef.min, max = funcDef.max, step = funcDef.step || 1;
  let v = min + Math.round((rawValue - min) / step) * step;
  if (v < min) v = min;
  if (v > max) v = max;
  return v;
}
function ecmSpcIdleSnapField(key) {
  const input = document.getElementById('spc-' + key + '-in');
  if (!input || input.value === '') return;
  input.value = ecmSpcIdleSnap(key, parseFloat(input.value));
}
function ecmSpcIdleStep(key, direction) {
  const funcDef = (ecmSpc.cat.functions || []).find(f => f.key === key);
  const input = document.getElementById('spc-' + key + '-in');
  if (!funcDef || !input) return;
  const base = input.value === '' ? funcDef.min : parseFloat(input.value);
  input.value = ecmSpcIdleSnap(key, ecmSpcIdleSnap(key, base) + direction * (funcDef.step || 1));
}
async function ecmSpcSetIdle(key) {
  const input = document.getElementById('spc-' + key + '-in');
  if (!input || input.value === '') { spcMsg('Enter an idle speed first', false); return; }
  const snapped = ecmSpcIdleSnap(key, parseFloat(input.value));
  input.value = snapped;   // show the adjusted value in the field too
  if (!(await hdConfirm(`Write idle speed ${snapped} rpm to the ECM?`, 'Set idle speed'))) return;
  ecmSpcRun(key, snapped);
}

function ecmSpcExit(manual) {
  if (!ecmSpc.open && !manual) return;
  if (!manual) { ecmSpc.open = false; ecmSpc.ended = false; }
  if (ecmSpc.poll) { clearInterval(ecmSpc.poll); ecmSpc.poll = null; }
  fetch(ecmSpc.base + '/exit', { method: 'POST', keepalive: true }).catch(() => {});
  if (manual) spcMsg(`Session ended - default session restored on the ${spcModuleName()}.`, true);
}

// ----------------------------------------------------------------------------
// ESP32 System Status Modal (Upper-Right Button near Settings)
// ----------------------------------------------------------------------------
function openStatusModal() {
  const modal = document.getElementById('modal-status');
  if (modal) modal.classList.add('show');
  refreshStatusModal();
}

function closeStatusModal() {
  const modal = document.getElementById('modal-status');
  if (modal) modal.classList.remove('show');
}

function applyStatusTelemetry(statusData) {
  if (!statusData) return;
  currentWifiMode = statusData.wifi_mode || 'STA';
  currentWifiRssi = statusData.rssi !== undefined ? statusData.rssi : -48;

  const bottomWifiModeText = document.getElementById('bottom-wifi-mode-text');
  const bottomWifiSignalText = document.getElementById('bottom-wifi-signal-text');
  if (bottomWifiModeText) bottomWifiModeText.textContent = currentWifiMode === 'STA' ? 'STA' : 'AP';
  if (bottomWifiSignalText) bottomWifiSignalText.textContent = currentWifiMode === 'STA' ? `${currentWifiRssi} dBm` : 'Open';

  const stIp = document.getElementById('st-ip');
  if (stIp && statusData.ip) stIp.textContent = statusData.ip;

  const stSsid = document.getElementById('st-ssid');
  if (stSsid && statusData.wifi_ssid) stSsid.textContent = statusData.wifi_ssid;

  const stWifiMode = document.getElementById('st-wifi-mode');
  if (stWifiMode) {
    const modeLabel = currentWifiMode === 'AP' ? 'Access Point (Open)' : `Station (STA) · ${currentWifiRssi} dBm`;
    stWifiMode.textContent = modeLabel;
  }

  const heapBytes = statusData.free_heap !== undefined ? statusData.free_heap : statusData.heap;
  const stHeap = document.getElementById('st-heap');
  if (stHeap && heapBytes !== undefined) {
    stHeap.textContent = `${(heapBytes / 1024).toFixed(1)} KB Free`;
  }

  const stCpu = document.getElementById('st-cpu');
  if (stCpu && statusData.cpu_mhz) stCpu.textContent = `${statusData.cpu_mhz} MHz`;

  const stUptime = document.getElementById('st-uptime');
  if (stUptime && statusData.uptime !== undefined) stUptime.textContent = statusData.uptime;

  const stMac = document.getElementById('st-mac');
  if (stMac && statusData.mac) stMac.textContent = `MAC: ${statusData.mac}`;

  const stFlash = document.getElementById('st-flash');
  if (stFlash && statusData.flash) {
    const flashKb = Math.round(statusData.flash / 1024);
    const flashMb = (statusData.flash / (1024 * 1024)).toFixed(0);
    stFlash.textContent = `${flashKb} KB (${flashMb} MB)`;
  }

  if (statusData.sketch_size && statusData.ota_slot_size) {
    const usedMb = (statusData.sketch_size / (1024 * 1024)).toFixed(2);
    const slotMb = (statusData.ota_slot_size / (1024 * 1024)).toFixed(2);
    const usagePercent = ((statusData.sketch_size / statusData.ota_slot_size) * 100).toFixed(1);
    const usageText = `${usagePercent}% (${usedMb} / ${slotMb} MB)`;
    const stAppPct = document.getElementById('st-app-pct');
    const stAppBadge = document.getElementById('st-app-badge');
    if (stAppPct) stAppPct.textContent = usageText;
    if (stAppBadge) stAppBadge.textContent = `App Flash: ${usageText}`;
  }

  const stCanBaud = document.getElementById('st-can-baud');
  if (stCanBaud) {
    const kbps = statusData.can_bitrate || 500;
    const busSt = statusData.bus_state || 'RUNNING';
    stCanBaud.textContent = `${kbps} kbps · ${busSt}`;
  }

  const stCanCounters = document.getElementById('st-can-counters');
  if (stCanCounters) {
    stCanCounters.innerHTML = `RX: <strong>${statusData.can_rx || 0}</strong> &middot; TX: <strong>${statusData.can_tx || 0}</strong> &middot; Err: <strong>${statusData.can_err || 0}</strong>`;
  }

  const stUdsCounters = document.getElementById('st-uds-counters');
  if (stUdsCounters) {
    stUdsCounters.textContent = `UDS Q/R: ${statusData.uds_queries || 0} / ${statusData.uds_replies || 0} · Filter: 0x7C0-0x7FF`;
  }

  const stNodeBadge = document.getElementById('st-node-badge');
  if (stNodeBadge && statusData.node_name) stNodeBadge.textContent = statusData.node_name;

  const stNvsSsid = document.getElementById('st-nvs-ssid');
  if (stNvsSsid && (statusData.nvs_ssid || statusData.wifi_ssid)) stNvsSsid.textContent = statusData.nvs_ssid || statusData.wifi_ssid;

  // CAN transceiver chip type & hardware pinout overview (Status modal)
  const isPlainChip = statusData.can_chip === 'plain';
  const stCanChip = document.getElementById('st-can-chip');
  if (stCanChip && statusData.can_chip) stCanChip.textContent = isPlainChip ? 'Plain (TX/RX only)' : 'TJA1043';

  const stCanTxRx = document.getElementById('st-can-tx-rx');
  if (stCanTxRx && statusData.can_tx_pin !== undefined && statusData.can_rx_pin !== undefined) {
    stCanTxRx.textContent = `GPIO ${statusData.can_tx_pin} / GPIO ${statusData.can_rx_pin}`;
  }

  const stTjaStbEnRow = document.getElementById('st-tja-stb-en-row');
  const stTjaStbEn = document.getElementById('st-tja-stb-en');
  if (stTjaStbEn && statusData.tja_stb_pin !== undefined && statusData.tja_en_pin !== undefined) {
    stTjaStbEn.textContent = `GPIO ${statusData.tja_stb_pin} / GPIO ${statusData.tja_en_pin}`;
  }
  // STB/EN control only exists on the TJA1043 - hide the row entirely in plain mode.
  if (stTjaStbEnRow) stTjaStbEnRow.style.display = isPlainChip ? 'none' : '';

  const stTjaErrLedRow = document.getElementById('st-tja-err-led-row');
  const stTjaErrLedLbl = document.getElementById('st-tja-err-led-lbl');
  const stTjaErrLed = document.getElementById('st-tja-err-led');
  if (statusData.can_chip) {
    if (isPlainChip) {
      // No ERR fault pin in plain mode - relabel the row to show only the activity LED pin.
      if (stTjaErrLedLbl) stTjaErrLedLbl.textContent = 'CAN Activity LED:';
      if (stTjaErrLed && statusData.tja_led_pin !== undefined) stTjaErrLed.textContent = `GPIO ${statusData.tja_led_pin}`;
    } else {
      if (stTjaErrLedLbl) stTjaErrLedLbl.textContent = 'TJA1043 ERR / LED:';
      if (stTjaErrLed && statusData.tja_err_pin !== undefined && statusData.tja_led_pin !== undefined) {
        stTjaErrLed.textContent = `GPIO ${statusData.tja_err_pin} / GPIO ${statusData.tja_led_pin}`;
      }
    }
  }
  if (stTjaErrLedRow) stTjaErrLedRow.style.display = '';

  if (statusData.fw_version && statusData.fw_build) {
    fwVersion = String(statusData.fw_version);
    fwBuild = String(statusData.fw_build);
    applyFwIdentity();
  }
}

function refreshStatusModal() {
  fetch('/api/status')
    .then(response => response.json())
    .then(data => applyStatusTelemetry(data))
    .catch(() => {});
}

// ----------------------------------------------------------------------------
// Themed Confirmation Dialog (in-app replacement for native browser
// confirm()/alert() popups, used before warning/acknowledge-gated functions
// such as ABS bleeding). Returns a Promise<boolean> resolved true/false.
// ----------------------------------------------------------------------------
let _hdConfirmResolve = null;
function hdConfirm(message, title, isHtml) {
  return new Promise((resolve) => {
    _hdConfirmResolve = resolve;
    const titleEl = document.getElementById('confirm-modal-title');
    const msgEl = document.getElementById('confirm-modal-message');
    if (titleEl) titleEl.textContent = title || 'Confirm';
    if (msgEl) {
      if (isHtml) {
        msgEl.innerHTML = message || '';
      } else {
        msgEl.textContent = message || '';
      }
    }
    const modal = document.getElementById('modal-confirm');
    if (modal) modal.classList.add('show');
  });
}
function hdConfirmResolve(result) {
  const modal = document.getElementById('modal-confirm');
  if (modal) modal.classList.remove('show');
  const resolver = _hdConfirmResolve;
  _hdConfirmResolve = null;
  if (resolver) resolver(result);
}

// Single-button acknowledge popup, used to report the result of a special-function operation. `ok` tints the
// title (green on success, red on failure). Returns a Promise resolved when the user presses OK / closes it.
let _hdAlertResolve = null;
function hdAlert(message, ok, title) {
  return new Promise((resolve) => {
    _hdAlertResolve = resolve;
    const titleEl = document.getElementById('alert-modal-title');
    const msgEl = document.getElementById('alert-modal-message');
    if (titleEl) { titleEl.textContent = title || (ok === false ? 'Operation failed' : 'Operation complete'); titleEl.style.color = ok === false ? 'var(--red, #ef4444)' : 'var(--green, #22c55e)'; }
    if (msgEl) msgEl.textContent = message || '';
    const modal = document.getElementById('modal-alert');
    if (modal) modal.classList.add('show');
  });
}
function hdAlertResolve() {
  const modal = document.getElementById('modal-alert');
  if (modal) modal.classList.remove('show');
  const resolver = _hdAlertResolve;
  _hdAlertResolve = null;
  if (resolver) resolver();
}

// Busy/waiting popup (no buttons, not dismissible) - shown while a confirmed action's UDS commands are
// actually running, between the "place FOB" / confirm popup and the result popup, so the operator sees the
// same kind of popup feedback the whole way through instead of an inline status line going quiet mid-flow.
// Always pair with hdWaitClose() once the operation's response comes back (success or failure).
function hdWait(message, title) {
  const titleEl = document.getElementById('wait-modal-title');
  const msgEl = document.getElementById('wait-modal-message');
  if (titleEl) titleEl.textContent = title || 'Please wait';
  if (msgEl) msgEl.textContent = message || 'Working...';
  const modal = document.getElementById('modal-wait');
  if (modal) modal.classList.add('show');
}
function hdWaitClose() {
  const modal = document.getElementById('modal-wait');
  if (modal) modal.classList.remove('show');
}

// ----------------------------------------------------------------------------
// Settings & Maintenance
// ----------------------------------------------------------------------------
function handleCanChipTypeChange() {
  const chipSelect = document.getElementById('cfg-can-chip');
  const tjaPinsSection = document.getElementById('cfg-tja-pins-section');
  if (!chipSelect || !tjaPinsSection) return;
  // STB/EN/ERR control lines only exist on the TJA1043 - hide them entirely for a plain transceiver.
  tjaPinsSection.style.display = (chipSelect.value === 'plain') ? 'none' : '';
}

function openSettingsModal() {
  document.getElementById('modal-settings').classList.add('show');
  fetch('/api/config')
    .then(response => response.json())
    .then(cfg => {
      const setVal = (id, value) => {
        const el = document.getElementById(id);
        if (el && value !== undefined && value !== null) el.value = value;
      };
      setVal('cfg-device-name', cfg.node_name);
      setVal('cfg-wifi-mode', cfg.wifi_mode);
      setVal('cfg-sta-ssid', cfg.wifi_ssid);
      setVal('cfg-ap-ssid', cfg.ap_ssid);
      setVal('cfg-can-bitrate', cfg.can_bitrate);
      setVal('cfg-can-chip', cfg.can_chip);
      setVal('cfg-can-tx-pin', cfg.can_tx_pin);
      setVal('cfg-can-rx-pin', cfg.can_rx_pin);
      setVal('cfg-tja-stb-pin', cfg.tja_stb_pin);
      setVal('cfg-tja-en-pin', cfg.tja_en_pin);
      setVal('cfg-tja-err-pin', cfg.tja_err_pin);
      // Passwords are intentionally never returned by the server (write-only fields) -
      // leave cfg-sta-pass / any AP password field exactly as the static HTML has it.
      handleCanChipTypeChange();
    })
    .catch(() => {});
  canLogFlagRefresh();    // sync the CAN traffic log checkbox
  devTestFlagRefresh();   // sync the developer test checkbox
}
function closeSettingsModal() {
  document.getElementById('modal-settings').classList.remove('show');
}

// (The RADIO 0x27 unlock is always performed now - the radio needs Security Access for writes, so the
// diagnostic enable/disable switch was removed. Keys come from the captured seed/key table in rad_special.cpp.)

// ---- CAN/UDS traffic log on serial console (persisted to NVS, default OFF) ----
// When off, the firmware does not print [UDS-TX]/[UDS-RX]/[ISOTP]/[UDS-SCAN] lines (they flood the console
// and can disturb timing). When on, they are printed. Saved to NVS.
function applyCanLogFlag(enabled) {
  const chk = document.getElementById('cfg-canlog-enabled');
  if (chk) chk.checked = !!enabled;
}
function canLogFlagRefresh() {
  fetch('/api/flags', { cache: 'no-store' }).then(r => r.json()).then(f => applyCanLogFlag(!!f.canlog_enabled)).catch(() => {});
}
function handleCanLogToggle() {
  const chk = document.getElementById('cfg-canlog-enabled');
  const en = chk && chk.checked ? '1' : '0';
  fetch('/api/settings/canlog?enabled=' + en, { method: 'POST', cache: 'no-store' })
    .then(r => r.json()).then(f => applyCanLogFlag(!!f.canlog_enabled)).catch(() => {});
}

// ---- Developer test mode (persisted to NVS, default OFF) ----
// When on, the ABS bleed brake-pressure prompts (apply/release the brake, pump cycle) also show a
// "Continue (debug)" button that skips the live pressure/pump check - see procRender(). Does not change
// what the firmware sends; it only lets the operator advance the on-screen step without a bike attached.
let hdDevTestEnabled = false;
function applyDevTestFlag(enabled) {
  hdDevTestEnabled = !!enabled;
  const chk = document.getElementById('cfg-devtest-enabled');
  if (chk) chk.checked = hdDevTestEnabled;
}
function devTestFlagRefresh() {
  fetch('/api/flags', { cache: 'no-store' }).then(r => r.json()).then(f => applyDevTestFlag(!!f.devtest_enabled)).catch(() => {});
}
function handleDevTestToggle() {
  const chk = document.getElementById('cfg-devtest-enabled');
  const en = chk && chk.checked ? '1' : '0';
  fetch('/api/settings/devtest?enabled=' + en, { method: 'POST', cache: 'no-store' })
    .then(r => r.json()).then(f => applyDevTestFlag(!!f.devtest_enabled)).catch(() => {});
}

// ---- DTC reference lookup (offline, 1,685-code English database) ----
function dtcRefLookup() {
  const inEl = document.getElementById('dtc-ref-code');
  const outEl = document.getElementById('dtc-ref-result');
  if (!inEl || !outEl) return;
  const code = (inEl.value || '').trim().toUpperCase();
  if (!code) { outEl.textContent = '—'; return; }
  outEl.textContent = 'Looking up...';
  fetch('/api/dtc/lookup?code=' + encodeURIComponent(code), { cache: 'no-store' })
    .then(r => r.json())
    .then(j => { outEl.innerHTML = '<strong>' + spcEsc(j.code) + '</strong> &mdash; ' + spcEsc(j.desc || 'not found'); })
    .catch(() => { outEl.textContent = 'lookup failed'; });
}

// ---- Vehicle info: odometer (km + miles) + VIN, read-only ----
function vehicleInfoRead() {
  const odoEl = document.getElementById('veh-odo');
  const vinEl = document.getElementById('veh-vin');
  if (odoEl) odoEl.textContent = 'Odometer: reading...';
  if (vinEl) vinEl.textContent = 'VIN: reading...';
  return fetch('/api/vehicle/info', { cache: 'no-store' })
    .then(r => r.json())
    .then(j => {
      if (odoEl) odoEl.innerHTML = (j && j.km != null)
        ? 'Odometer: <strong>' + Number(j.km).toLocaleString() + ' km</strong> (' + Number(j.mi).toLocaleString() + ' mi)'
        : 'Odometer: <span class="spc-err">not available from speedometer (0x7F2)</span>';
      if (vinEl) vinEl.innerHTML = (j && j.vin) ? 'VIN: <strong>' + spcEsc(j.vin) + '</strong>' : 'VIN: <span class="spc-err">not read</span>';
    })
    .catch(() => { if (odoEl) odoEl.textContent = 'Odometer: read failed'; if (vinEl) vinEl.textContent = 'VIN: read failed'; });
}

// ---- Read current FOB assignment (BCM, read-only) ----
function fobBlocks(hex) {
  if (!hex) return '(none)';
  return hex.match(/.{1,4}/g).join('-');   // group into 2-byte blocks for readability
}
// Session-original FOB serials: captured on the FIRST successful read this session so "Restore original"
// can refill the input field with them (it does NOT write to the module - same as the other BCM menus).
let fobOrig = { f1: null, f2: null };

// Confirmed-read tag: shows whether two reads agreed over the flaky WiFi bridge.
function fobConfTag(ok) { return ok ? '' : ' <span class="spc-warn">(unconfirmed &mdash; read again)</span>'; }

// One shared read against /api/bcm/fob_status (now retries + confirms server-side). Returns the parsed JSON.
function fobFetchStatus() {
  return fetch('/api/bcm/fob_status', { cache: 'no-store' }).then(r => r.json()).catch(() => null);
}

// Reflects one /api/bcm/fob_status result into every FOB card currently in the DOM (the read-only "Key FOB
// status" card, and the "New FOB serial" input(s) of both assignment cards) - a single BCM read serves all
// three, instead of each card fetching the same serials over again. Also prefills the "New ... serial" input
// with the value just read (same as every other special-function Read), so Assign starts from the current
// serial instead of a blank field.
function fobApplyStatus(j) {
  if (fobOrig.f1 === null) fobOrig.f1 = j.fob1 || '';
  if (fobOrig.f2 === null) fobOrig.f2 = j.fob2 || '';
  const f1text = j.fob1 ? fobBlocks(j.fob1) : 'not assigned';
  const f2text = j.fob2 ? fobBlocks(j.fob2) : 'not assigned';
  const f1in = j.fob1 ? fobBlocks(j.fob1) : '';
  const f2in = j.fob2 ? fobBlocks(j.fob2) : '';

  const statusEl = document.getElementById('fob-read-result') || document.getElementById('veh-fob');
  if (statusEl) statusEl.innerHTML = 'FOB 1: <strong>' + spcEsc(f1text) + '</strong>' + fobConfTag(j.conf1) +
                                      '<br>FOB 2: <strong>' + spcEsc(f2text) + '</strong>' + fobConfTag(j.conf2);

  // "Original" - shown here (not as a separate popup) so it is visible the whole time the operator is
  // editing the "New ... serial" field below it.
  const cur1 = document.getElementById('fob1-cur'), in1 = document.getElementById('fob1-in');
  if (cur1) cur1.innerHTML = 'Original: <strong>' + spcEsc(f1text) + '</strong>' + fobConfTag(j.conf1);
  if (in1) in1.value = f1in;

  const c2a = document.getElementById('fob2a-cur'), i2a = document.getElementById('fob2a-in');
  const c2b = document.getElementById('fob2b-cur'), i2b = document.getElementById('fob2b-in');
  if (c2a) c2a.innerHTML = 'FOB 1 Original: <strong>' + spcEsc(f1text) + '</strong>' + fobConfTag(j.conf1);
  if (i2a) i2a.value = f1in;
  if (c2b) c2b.innerHTML = 'FOB 2 Original: <strong>' + spcEsc(f2text) + '</strong>' + fobConfTag(j.conf2);
  if (i2b) i2b.value = f2in;
}

function fobApplyError(msg) {
  const statusEl = document.getElementById('fob-read-result') || document.getElementById('veh-fob');
  if (statusEl) statusEl.innerHTML = 'FOB 1: <span class="spc-err">' + spcEsc(msg) + '</span><br>FOB 2: <span class="spc-err">' + spcEsc(msg) + '</span>';
  const cur1 = document.getElementById('fob1-cur');
  if (cur1) cur1.innerHTML = 'Original: <span class="spc-err">' + spcEsc(msg) + '</span>';
  const c2a = document.getElementById('fob2a-cur');
  if (c2a) c2a.innerHTML = 'FOB 1 Original: <span class="spc-err">' + spcEsc(msg) + '</span>';
  const c2b = document.getElementById('fob2b-cur');
  if (c2b) c2b.innerHTML = 'FOB 2 Original: <span class="spc-err">' + spcEsc(msg) + '</span>';
}

function fobSetReading() {
  const statusEl = document.getElementById('fob-read-result') || document.getElementById('veh-fob');
  if (statusEl) statusEl.innerHTML = 'FOB 1: <em>reading...</em><br>FOB 2: <em>reading...</em>';
  const cur1 = document.getElementById('fob1-cur');
  if (cur1) cur1.textContent = 'Original: reading...';
  const c2a = document.getElementById('fob2a-cur');
  if (c2a) c2a.textContent = 'FOB 1 Original: reading...';
  const c2b = document.getElementById('fob2b-cur');
  if (c2b) c2b.textContent = 'FOB 2 Original: reading...';
}

// One BCM read, reflected into whichever of the three FOB cards (status / assignment-1 / assignment-2) are
// present on the page. Used by the individual Read button on any of the three cards AND by "Read all" - so
// clicking Read on one card, or running Read all, never re-reads the same FOB serials from the BCM twice.
function fobReadAll() {
  fobSetReading();
  return fobFetchStatus().then(j => {
    if (!j || !j.ok) { fobApplyError('read failed'); return; }
    fobApplyStatus(j);
  });
}

function fobStatusRead() { return fobReadAll(); }

// Restrict a FOB-serial input to hex and auto-group it as XXXX-XXXX-... (2-byte blocks), max 12 bytes.
function fobSerialInput(el) {
  let v = el.value.toUpperCase().replace(/[^0-9A-F]/g, '');
  if (v.length > 24) v = v.slice(0, 24);
  const groups = v.match(/.{1,4}/g);
  el.value = groups ? groups.join('-') : v;
}

// Place-FOB prompt shared by both assignment cards: a single-button
// "Place FOB close to the Motorcycle." acknowledgement shown AFTER the assign confirmation and BEFORE any
// write (the FOB must stay near the antenna through activation, so this is where the operator holds it up).
function fobPlacePrompt(what) {
  return hdAlert(`Place FOB close to the Motorcycle. Hold ${what} against the fob antenna, then continue.`, true, 'Place FOB close to the Motorcycle');
}

// ---- Key FOB assignment (1 FOB) ----
function fob1Read() { return fobReadAll(); }
async function fob1Assign() {
  const inp = document.getElementById('fob1-in');
  const raw = (inp ? inp.value : '').replace(/[^0-9A-Fa-f]/g, '');
  if (!/^[0-9A-Fa-f]{24}$/.test(raw)) {
    spcMsg('FOB serial: enter 6 groups of 4 hex (12 bytes), as printed on the FOB', false); return;
  }
  const newSerial = fobBlocks(raw.toUpperCase());
  const origSerial = fobOrig.f1 ? fobBlocks(fobOrig.f1) : '(none)';
  if (!(await hdConfirm(`Do you really want to assign Key FOB with serial\nNew: ${newSerial}\nOld: ${origSerial}`, 'Assign key FOB'))) return;
  await fobPlacePrompt('the FOB');
  await fobWriteOne(raw, null, 'Programming the FOB... (writing serial, then activating it on the BCM)');
}

// ---- Key FOB assignment (2 FOBs) ----
// Run as two full, independent, step-by-step passes - confirm FOB 1 -> place FOB 1 -> wait/write FOB 1 ->
// result, THEN confirm FOB 2 -> place FOB 2 -> wait/write FOB 2 -> result - same shape as the single-FOB
// card above (PASS 1 complete end-to-end before PASS 2 starts). If FOB 1's pass fails, FOB 2's pass is not
// attempted (a failed later pass never undoes an earlier one, but there's no reason to continue onto FOB 2
// after FOB 1 just failed).
function fob2Read() { return fobReadAll(); }
async function fob2Assign() {
  const r1 = (document.getElementById('fob2a-in') || {}).value || '';
  const r2 = (document.getElementById('fob2b-in') || {}).value || '';
  const h1 = r1.replace(/[^0-9A-Fa-f]/g, ''), h2 = r2.replace(/[^0-9A-Fa-f]/g, '');
  const bad = s => !/^[0-9A-Fa-f]{24}$/.test(s);
  if (bad(h1) || bad(h2)) { spcMsg('Both FOB serials: enter 6 groups of 4 hex (12 bytes) each', false); return; }

  const new1 = fobBlocks(h1.toUpperCase()), orig1 = fobOrig.f1 ? fobBlocks(fobOrig.f1) : '(none)';
  if (!(await hdConfirm(`Do you really want to assign Key FOB with serial\nNew: ${new1}\nOld: ${orig1}`, 'Assign key FOB 1 of 2'))) return;
  await fobPlacePrompt('the first FOB');
  const result1 = await fobWriteOne(h1, null, 'Programming FOB 1 of 2... (writing serial, then activating it on the BCM)');
  if (!result1 || !result1.ok) return;   // its own failure popup was already shown by fobWriteOne

  const new2 = fobBlocks(h2.toUpperCase()), orig2 = fobOrig.f2 ? fobBlocks(fobOrig.f2) : '(none)';
  if (!(await hdConfirm(`Do you really want to assign Key FOB with serial\nNew: ${new2}\nOld: ${orig2}`, 'Assign key FOB 2 of 2'))) return;
  await fobPlacePrompt('the second FOB');
  await fobWriteOne(null, h2, 'Programming FOB 2 of 2... (writing serial, then activating it on the BCM)');
}

// One FOB-slot write pass: shows the "Please wait" popup while the firmware runs the unlock/write/activate
// UDS sequence (/api/bcm/fob_write accepts s1 and/or s2, each run as its own independent pass - see the
// firmware comment there), closes it on response, reports the result the same way every other special
// function does (status line + acknowledge popup), and reflects the confirmed read-back into every FOB
// card on the page (fob_write's response has the same shape as fob_status, so one fobApplyStatus() call
// keeps the status card and the OTHER assignment card in sync too, not just the one that triggered it).
// Returns the parsed response ({ok:false} on a link error) so callers can stop a multi-FOB sequence on failure.
async function fobWriteOne(s1, s2, waitMessage) {
  hdWait(waitMessage || 'Working... (talking to the BCM)', 'Please wait');
  let body = '';
  if (s1) body += 's1=' + encodeURIComponent(s1);
  if (s2) body += (body ? '&' : '') + 's2=' + encodeURIComponent(s2);
  let j;
  try {
    const resp = await fetch('/api/bcm/fob_write', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
    j = await resp.json();
  } catch (e) { j = null; }
  hdWaitClose();
  if (!j) { spcMsg('assignment failed (link error)', false); await hdAlert('No response from the ESP32', false); return { ok: false }; }
  spcMsg(j.msg || (j.ok ? 'FOB assigned' : 'assignment failed'), j.ok);
  await hdAlert(j.msg || (j.ok ? 'FOB assigned' : 'assignment failed'), j.ok);
  fobApplyStatus(j);
  return j;
}

// Restore original: refill the input field(s) with the serial read at the start of this session. No write.
function fobRestore(which) {
  if (which === 1) {
    const inp = document.getElementById('fob1-in');
    if (inp) inp.value = fobOrig.f1 ? fobBlocks(fobOrig.f1) : '';
    spcMsg(fobOrig.f1 === null ? 'Read the current FOB first.' : 'Restored original FOB 1 serial into the field.', fobOrig.f1 !== null);
  } else {
    const i1 = document.getElementById('fob2a-in'), i2 = document.getElementById('fob2b-in');
    if (i1) i1.value = fobOrig.f1 ? fobBlocks(fobOrig.f1) : '';
    if (i2) i2.value = fobOrig.f2 ? fobBlocks(fobOrig.f2) : '';
    spcMsg((fobOrig.f1 === null && fobOrig.f2 === null) ? 'Read the current FOBs first.' : 'Restored original FOB serials into the fields.', !(fobOrig.f1 === null && fobOrig.f2 === null));
  }
}

// ---- Read current anti-theft PIN (BCM, unlocks then reads DID 0141) ----
function pinRead() {
  const el = document.getElementById('veh-pin');
  if (el) el.textContent = 'PIN: reading...';
  fetch('/api/bcm/pin_read', { cache: 'no-store' })
    .then(r => r.json())
    .then(j => {
      if (!el) return;
      if (!j || !j.ok) { el.innerHTML = 'PIN: <span class="spc-err">read failed</span>'; return; }
      if (!j.unlocked) { el.innerHTML = 'PIN: <span class="spc-err">BCM unlock rejected</span> (verify key/log)'; return; }
      el.innerHTML = 'PIN: <strong>' + spcEsc(j.pin || '(empty)') + '</strong>';
    })
    .catch(() => { if (el) el.textContent = 'PIN: read failed'; });
}

// (The seed/key pair store editor was removed - keys are now computed per module in firmware, <module>_algo.cpp.)
function handleSaveSettings(e) {
  e.preventDefault();
  const nodeName = (document.getElementById('cfg-device-name') || {}).value || 'ESP32 HD';
  const wifiMode = (document.getElementById('cfg-wifi-mode') || {}).value || 'STA';
  const staSsid = (document.getElementById('cfg-sta-ssid') || {}).value || 'ESP32-HD';
  const staPass = (document.getElementById('cfg-sta-pass') || {}).value || '1122334455';
  const apSsid = (document.getElementById('cfg-ap-ssid') || {}).value || 'ESP-32-HD';
  const bitrate = (document.getElementById('cfg-can-bitrate') || {}).value || '500000';
  const canChip = (document.getElementById('cfg-can-chip') || {}).value || 'tja1043';
  const canTxPin = (document.getElementById('cfg-can-tx-pin') || {}).value;
  const canRxPin = (document.getElementById('cfg-can-rx-pin') || {}).value;
  const tjaStbPin = (document.getElementById('cfg-tja-stb-pin') || {}).value;
  const tjaEnPin = (document.getElementById('cfg-tja-en-pin') || {}).value;
  const tjaErrPin = (document.getElementById('cfg-tja-err-pin') || {}).value;

  let body = `node_name=${encodeURIComponent(nodeName)}&wifi_mode=${encodeURIComponent(wifiMode)}&wifi_ssid=${encodeURIComponent(staSsid)}&wifi_pass=${encodeURIComponent(staPass)}&ap_ssid=${encodeURIComponent(apSsid)}&can_bitrate=${encodeURIComponent(bitrate)}&can_chip=${encodeURIComponent(canChip)}`;
  if (canTxPin !== '' && canTxPin !== undefined) body += `&can_tx_pin=${encodeURIComponent(canTxPin)}`;
  if (canRxPin !== '' && canRxPin !== undefined) body += `&can_rx_pin=${encodeURIComponent(canRxPin)}`;
  if (tjaStbPin !== '' && tjaStbPin !== undefined) body += `&tja_stb_pin=${encodeURIComponent(tjaStbPin)}`;
  if (tjaEnPin !== '' && tjaEnPin !== undefined) body += `&tja_en_pin=${encodeURIComponent(tjaEnPin)}`;
  if (tjaErrPin !== '' && tjaErrPin !== undefined) body += `&tja_err_pin=${encodeURIComponent(tjaErrPin)}`;

  closeSettingsModal();
  fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: body
  }).catch(() => {});
  showRebootOverlay('Saving settings and rebooting the ESP32…');
  return false;
}

// Full-screen "rebooting" overlay (like OpenWrt LuCI): show a message, count down ~10 s while the device
// restarts, then refresh. opts.note adds a hint line; opts.pollPath is the URL polled to detect the device is
// back (default /api/status). opts.poll:false just reloads after the countdown (used for Safe Boot, which
// comes up as a SEPARATE recovery Wi-Fi network and does not serve /api/status).
function showRebootOverlay(message, opts) {
  opts = opts || {};
  const pollPath = opts.pollPath || '/api/status';
  let ov = document.getElementById('reboot-overlay');
  if (!ov) { ov = document.createElement('div'); ov.id = 'reboot-overlay'; document.body.appendChild(ov); }
  ov.setAttribute('style', 'position:fixed;inset:0;z-index:99999;display:flex;flex-direction:column;' +
    'align-items:center;justify-content:center;gap:16px;text-align:center;padding:24px;' +
    'background:rgba(8,10,14,0.92);color:#e8edf2;font-size:15px;backdrop-filter:blur(2px)');
  ov.innerHTML = '<div style="width:46px;height:46px;border:4px solid rgba(255,255,255,0.18);' +
    'border-top-color:#ff8a00;border-radius:50%;animation:hd-spin 0.9s linear infinite"></div>' +
    '<div style="font-weight:700;font-size:17px">' + spcEsc(message) + '</div>' +
    '<div id="reboot-countdown" style="opacity:0.85">Refreshing in 10 s…</div>' +
    (opts.note ? '<div style="opacity:0.8;font-size:12px;max-width:340px">' + spcEsc(opts.note) + '</div>' : '') +
    '<div style="opacity:0.6;font-size:12px">The page refreshes automatically.</div>';
  if (!document.getElementById('hd-spin-kf')) {
    const st = document.createElement('style'); st.id = 'hd-spin-kf';
    st.textContent = '@keyframes hd-spin{to{transform:rotate(360deg)}}'; document.head.appendChild(st);
  }
  let left = 10;
  const cd = () => { const el = document.getElementById('reboot-countdown'); if (el) el.textContent = left > 0 ? ('Refreshing in ' + left + ' s…') : 'Waiting for the device…'; };
  cd();
  const tick = setInterval(() => {
    left--; cd();
    if (left <= 0) { clearInterval(tick); if (opts.poll === false) { location.reload(); } else { waitForDevice(0); } }
  }, 1000);
  // After the countdown, poll until the device answers, then reload (or reload anyway after a cap).
  function waitForDevice(tries) {
    fetch(pollPath, { cache: 'no-store' })
      .then(r => { if (r && r.ok) { location.reload(); } else { throw 0; } })
      .catch(() => { if (tries >= 20) { location.reload(); } else { setTimeout(() => waitForDevice(tries + 1), 1500); } });
  }
}

function triggerReboot() {
  fetch('/api/system/reboot', { method: 'POST' }).catch(() => {});
  showRebootOverlay('Rebooting the ESP32…');
}
function triggerSafeBootMode() {
  fetch('/api/system/safeboot', { method: 'POST' }).catch(() => {});
  // Safe Boot restarts into a separate recovery Wi-Fi network and does not serve /api/status, so don't poll -
  // just refresh after the countdown; the page then loads the Safe Boot recovery UI (once connected to it).
  showRebootOverlay('Restarting into Safe Boot…', {
    poll: false,
    note: 'Safe Boot starts a separate recovery Wi-Fi network. If the page does not load, connect to the Safe Boot network, then it will open the recovery page.'
  });
}
function triggerFactoryReset() {
  fetch('/api/system/factory_reset', { method: 'POST' }).catch(() => {});
  showRebootOverlay('Factory reset — restarting the ESP32…');
}

// ----------------------------------------------------------------------------
// Seamless firmware update (Tasmota-style handoff)
// ----------------------------------------------------------------------------
// The main app runs from ota_0 and cannot erase/write the slot it is executing
// from, and a 4 MB flash has no room to stage a 2.8 MB copy. So instead of
// flashing here, we keep the chosen .bin in browser memory, reboot the device
// into Safe Boot (the recovery firmware in the factory partition, which CAN
// erase+write ota_0), wait for it to rejoin the SAME Wi-Fi at the SAME IP, then
// POST the file straight to Safe Boot's /update. One file pick, one upload,
// automatic reboot into the new app - the Safe Boot step is hidden from the user.
function promptFirmwareUpdate() {
  let inp = document.getElementById('hd-fw-file');
  if (!inp) {
    inp = document.createElement('input');
    inp.type = 'file'; inp.accept = '.bin,application/octet-stream'; inp.id = 'hd-fw-file';
    inp.style.display = 'none';
    inp.addEventListener('change', () => {
      const f = inp.files && inp.files[0];
      inp.value = '';                 // reset so the same file can be re-picked later
      if (f) confirmFirmwareUpdate(f);
    });
    document.body.appendChild(inp);
  }
  inp.click();
}

async function confirmFirmwareUpdate(file) {
  const mb = (file.size / 1048576).toFixed(2);
  const safeName = String(file.name).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const html =
    '<div class="confirm-modal-centered">' +
      'Flash <span class="confirm-fw-name">"' + safeName + '"</span> (' + mb + ' MB) as the new main firmware?<br><br>' +
      'The device restarts into the Safe Boot updater,<br>' +
      'writes the firmware, then reboots into the app automatically.<br><br>' +
      'Keep this page open and stay on the same Wi-Fi until it finishes.' +
    '</div>';
  const ok = await hdConfirm(html, 'Update firmware', true);
  if (ok) runFirmwareHandoff(file);
}

function hdFwOverlay() {
  if (!document.getElementById('hd-spin-kf')) {
    const st = document.createElement('style'); st.id = 'hd-spin-kf';
    st.textContent = '@keyframes hd-spin{to{transform:rotate(360deg)}}'; document.head.appendChild(st);
  }
  let ov = document.getElementById('hd-fw-overlay');
  if (!ov) { ov = document.createElement('div'); ov.id = 'hd-fw-overlay'; document.body.appendChild(ov); }
  ov.setAttribute('style', 'position:fixed;inset:0;z-index:99999;display:flex;flex-direction:column;' +
    'align-items:center;justify-content:center;gap:16px;text-align:center;padding:24px;' +
    'background:rgba(8,10,14,0.92);color:#e8edf2;font-size:15px;backdrop-filter:blur(2px)');
  ov.innerHTML =
    '<div style="width:46px;height:46px;border:4px solid rgba(255,255,255,0.18);' +
      'border-top-color:#ff8a00;border-radius:50%;animation:hd-spin 0.9s linear infinite"></div>' +
    '<div id="hd-fw-stage" style="font-weight:700;font-size:17px">Starting updater…</div>' +
    '<div id="hd-fw-sub" style="opacity:0.85;max-width:380px;line-height:1.4"></div>' +
    '<div style="width:300px;max-width:80vw;height:10px;border-radius:6px;background:rgba(255,255,255,0.12);overflow:hidden">' +
      '<div id="hd-fw-bar" style="height:100%;width:0%;background:#ff8a00;transition:width 0.2s"></div>' +
    '</div>' +
    '<div id="hd-fw-pct" style="opacity:0.7;font-size:12px"></div>';
  const set = (id, prop, val) => { const e = document.getElementById(id); if (e) e[prop] = val; };
  return {
    stage: (t) => set('hd-fw-stage', 'textContent', t),
    sub:   (t) => set('hd-fw-sub', 'textContent', t || ''),
    bar:   (p) => { const b = document.getElementById('hd-fw-bar'); const x = document.getElementById('hd-fw-pct');
                    if (b) b.style.width = p + '%'; if (x) x.textContent = p > 0 ? (p + '%') : ''; },
    error: (msg) => {
      ov.innerHTML =
        '<div style="font-weight:700;font-size:17px;color:#ff6b6b">Update failed</div>' +
        '<div style="opacity:0.9;max-width:400px;line-height:1.45">' + spcEsc(msg) + '</div>' +
        '<button id="hd-fw-close" class="btn btn-secondary">Close</button>';
      const b = document.getElementById('hd-fw-close'); if (b) b.onclick = () => ov.remove();
    }
  };
}

function runFirmwareHandoff(file) {
  const ui = hdFwOverlay();
  const origin = location.origin;

  // 1) Ask the main app to reboot into Safe Boot (sets sb_flag + restarts).
  ui.stage('Restarting into the updater…');
  ui.sub('The device is rebooting into Safe Boot. Keep this page open.');
  fetch('/api/system/safeboot', { method: 'POST' }).catch(() => {});

  // 2) Poll for Safe Boot. Only Safe Boot serves /api/safeboot/info, so a 200
  //    there means the updater is up at the same origin and ready to receive.
  let waitTries = 0;
  const WAIT_MAX = 60;                 // ~3 s initial + 60 x 1.2 s ≈ up to ~75 s
  setTimeout(pollUpdater, 3000);
  function pollUpdater() {
    ui.stage('Waiting for the updater…');
    ui.sub('Safe Boot is starting (this takes ~10–20 s).');
    fetch('/api/safeboot/info', { cache: 'no-store' })
      .then(r => { if (r && r.ok) { uploadToUpdater(); } else { throw 0; } })
      .catch(() => {
        if (++waitTries >= WAIT_MAX) {
          ui.error('The Safe Boot updater did not answer at ' + origin + ' in time. It may have joined a ' +
            'different IP address on the network. Open the Safe Boot page directly and upload the file there.');
        } else {
          setTimeout(pollUpdater, 1200);
        }
      });
  }

  // 3) Upload the held .bin straight to Safe Boot's /update (field name 'update').
  function uploadToUpdater() {
    ui.stage('Uploading firmware…');
    ui.sub(file.name);
    ui.bar(0);
    const fd = new FormData();
    fd.append('update', file);
    const xhr = new XMLHttpRequest();
    let uploadComplete = false;
    xhr.open('POST', '/update', true);
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) ui.bar(Math.round((e.loaded / e.total) * 100)); };
    xhr.upload.onload = () => {
      uploadComplete = true; ui.bar(100);
      ui.stage('Finishing…'); ui.sub('Image sent — the updater is writing it to flash.');
    };
    xhr.onload = () => {
      if (xhr.status === 200) { afterUpload(); return; }
      let reason = '';
      try { reason = (JSON.parse(xhr.responseText) || {}).error || ''; } catch (e) {}
      // 413 (or a non-JSON body) almost always means a reverse proxy in front of the device
      // rejected the upload before it reached the updater - raise its max body size.
      const proxyHint = (xhr.status === 413 || (xhr.status >= 400 && !reason))
        ? ' This looks like a reverse proxy blocking the upload — raise its upload size limit (nginx: client_max_body_size 16m; and proxy_read_timeout 300s).'
        : '';
      ui.error('The updater rejected the firmware (HTTP ' + (xhr.status || '?') + (reason ? ', ' + reason : '') + ').' +
        proxyHint + ' The old firmware is unchanged — the device will return to the app.');
    };
    xhr.onerror = () => {
      // Whole image already sent => the device is flashing+rebooting and dropped the socket: treat as success.
      if (uploadComplete) { afterUpload(); return; }
      ui.error('Network error during upload before the image finished sending. Move closer to the Wi-Fi ' +
        'access point and try again — the old firmware is unchanged.');
    };
    xhr.send(fd);
  }

  // 4) Safe Boot writes ota_0 and reboots into the new app. Wait for the app to
  //    answer /api/status again at the same origin, then reload into it.
  let flashing = false;
  function afterUpload() {
    if (flashing) return; flashing = true;
    ui.stage('Flashing & rebooting…');
    ui.sub('The updater is writing the firmware and restarting into the app. This takes ~15–25 s.');
    ui.bar(100);
    let tries = 0;
    setTimeout(waitForApp, 6000);
    function waitForApp() {
      fetch('/api/status', { cache: 'no-store' })
        .then(r => { if (r && r.ok) { location.reload(); } else { throw 0; } })
        .catch(() => { if (++tries >= 50) { location.reload(); } else { setTimeout(waitForApp, 1500); } });
    }
  }
}

// ----------------------------------------------------------------------------
// Poll Status & Init
// ----------------------------------------------------------------------------
function pollStatus() {
  if (isDiagOperationRunning || isReadingEcuInfo || ecmStreamActive) return;
  fetch('/api/status')
    .then(response => response.json())
    .then(data => applyStatusTelemetry(data))
    .catch(() => {});
}

window.addEventListener('beforeunload', () => {
  if (currentActiveOperation === 'outputs') {
    cleanupModuleOutputs(false);
  }
});

window.addEventListener('DOMContentLoaded', () => {
  fetch('/api/uds/module/exit', { method: 'POST' }).catch(() => {});
  setTopScanModeActive('search');

  applyFwIdentity();

  initTelemetryWebSocket();
  pollStatus();
  setInterval(pollStatus, 5000);
  canLogFlagRefresh();    // sync the CAN traffic log checkbox per the saved NVS flag (default OFF)
  devTestFlagRefresh();   // sync the developer test checkbox per the saved NVS flag (default OFF)
});

