// Behaviour test for the Auto Brightness settings JS (appendConfigData) against a
// FAITHFUL replica of WLED's settings DOM + helpers (addField / addDD / addO / addI
// ported verbatim from WLED/wled00/data/settings_um.htm). The v1.0.1 replica was wrong —
// it omitted the hidden type-marker input WLED emits before EVERY field — so it passed a
// broken UI (orphaned labels). This one builds the DOM exactly as WLED does.
//
// The JS under test is EXTRACTED from usermod_v2_auto_brightness.cpp (the oappend(F(...))
// chunks, with the C++ Analog-Pin loop simulated for a classic ESP32), so this is a real
// regression test with no scratch dependency.
//   Run:  cd tools && npm i jsdom && node settings-ui.test.js
const { JSDOM } = require('jsdom');
const fs = require('fs');
const path = require('path');

// ---- extract the injected JS straight from the .cpp -----------------------
function extractInjectedJs() {
  const cpp = fs.readFileSync(path.join(__dirname, '..', 'usermod_v2_auto_brightness.cpp'), 'utf8');
  const s = cpp.indexOf('void appendConfigData()');
  const e = cpp.indexOf('\n  void addToConfig', s);
  const body = cpp.slice(s, e);
  let js = '';
  const re = /oappend\(F\("((?:[^"\\]|\\.)*)"\)\)/g;
  let m;
  while ((m = re.exec(body)) !== null) js += m[1].replace(/\\"/g, '"').replace(/\\\\/g, '\\');
  // simulate the C++ pin loop (classic ESP32 ADC1 GPIOs 32-39) after the "unused" option
  const pins = [32,33,34,35,36,37,38,39].map(g => `addOption(dd,'${g}','${g}');`).join('');
  return js.replace("addOption(dd,'unused','-1');", "addOption(dd,'unused','-1');" + pins);
}

// ---- config object exactly as addToConfig() emits it ----------------------
const cfg = {
  'Auto Brightness': {
    'Enabled': true,
    'Light Sensor': {
      'Source': 0, 'BH1750 Address': 35, 'Analog Pin': 34,
      'Cal Dark Raw': 200, 'Cal Dark Lux': 1, 'Cal Bright Raw': 3800, 'Cal Bright Lux': 1000,
    },
    'Brightness': {
      'Control Enabled': false, 'Lux Min': 1, 'Lux Max': 1000,
      'Brightness Min': 5, 'Brightness Max': 255, 'Smoothing': 70,
      'Update Interval': 2, 'Allow Manual Offset': true,
    },
    'Off When Dark': { 'Enabled': false, 'Off Below Lux': 5, 'On Above Lux': 20 },
    'MQTT & Home Assistant': {
      'Publish Illuminance': true, 'Publish Changes Only': true, 'Home Assistant Discovery': false,
    },
  },
};

const dom = new JSDOM('<body><form name="Sf"><div id="um"></div></form></body>', { runScripts: 'outside-only' });
const w = dom.window, d = w.document;
w.d = d;

// ---- addField(): builds urows exactly like settings_um.htm -----------------
function initCap(s){ return (''+s).replace(/[\W_]/g,' ').replace(/(^\w{1})|(\s+\w{1})/g, l=>l.toUpperCase()); }
function isO(o){ return o && typeof o === 'object' && !Array.isArray(o); }
let urows = '';
function addField(k,f,o){
  if (isO(o)) {
    urows += '<hr class="sml">';
    if (f!=='unknown' && !k.includes(':')) urows += `<p><u>${initCap(f)}</u></p>`;
    for (const [s,v] of Object.entries(o)) {
      if (f!=='unknown' && !k.includes(':')) addField(k+':'+f,s,v);
      else addField(k,s,v);
    }
  } else {
    var c, t = typeof o;
    switch (t) {
      case 'boolean': t='checkbox'; c='value="true"'+(o?' checked':''); break;
      case 'number':  c=`value="${o}"`; c+=' step="any" class="xxl"'; break;
      default:        t='text'; c=`value="${o}" style="width:250px;"`; break;
    }
    urows += ` ${initCap(f)} `;
    if (t=='checkbox') urows += `<input type="hidden" name="${k}:${f}" value="false">`;
    else               urows += `<input type="hidden" name="${k}:${f}" value="${t}">`;
    urows += `<input type="${t==='int'?'number':t}" name="${k}:${f}" ${c}><br>`;
  }
}
urows = '';
for (const [k,o] of Object.entries(cfg)) { urows += `<div class="sec"><h3>${k}</h3>`; addField(k,'unknown',o); urows += '</div>'; }
d.getElementById('um').innerHTML = urows;

// ---- addDD / addO / addI ported from settings_um.htm -----------------------
function addDD(um,fld){
  let sel = d.createElement('select');
  if (typeof fld === 'string') um += ':'+fld;
  let arr = d.getElementsByName(um);
  if (!arr || arr.length === 0) return null;
  let idx = (arr[0] && arr[0].type==='hidden')?1:0;
  let inp = arr[idx];
  if (inp && inp.tagName==='INPUT' && (inp.type==='text' || inp.type==='number')) {
    let v = inp.value;
    for (let i=0;i<inp.attributes.length;++i){ let att=inp.attributes[i];
      if (!['type','value','class','style','oninput','max','min'].includes(att.name)) sel.setAttribute(att.name, att.value); }
    sel.setAttribute('data-val', v);
    inp.parentElement.replaceChild(sel, inp);
    return sel;
  }
  return null;
}
function addO(sel,txt,val){ if(sel===null||sel===undefined)return; let opt=d.createElement('option'); opt.value=val; opt.text=txt; sel.appendChild(opt);
  for(let i=0;i<sel.childNodes.length;i++){ if(sel.childNodes[i].value==sel.dataset.val) sel.selectedIndex=i; } return opt; }
function addI(name,el,txt,txt2=''){ let obj=d.getElementsByName(name); if(!obj.length)return;
  if (typeof el==='string' && obj[0]) obj[0].placeholder=el;
  else if (obj[el]) { if(txt!=='') obj[el].insertAdjacentHTML('afterend','&nbsp;'+txt); if(txt2!=='') obj[el].insertAdjacentHTML('beforebegin', txt2+'&nbsp;'); } }
w.addDropdown = addDD; w.addOption = addO; w.addInfo = addI;
// jsdom lacks fetch; stub it (the JS calls it only on button clicks / refresh)
w.fetch = () => Promise.reject(new Error('no fetch in test'));

// ---- run the injected JS (extracted from the .cpp) -------------------------
const js = extractInjectedJs();
w.eval(js);

// ---- assertions ------------------------------------------------------------
let fails = 0;
const ok = (c,m)=>{ console.log((c?'PASS':'FAIL')+'  '+m); if(!c) fails++; };
const secs = [...d.getElementsByClassName('sec')];
const sec = secs.find(s => s.querySelector('h3') && s.querySelector('h3').textContent==='Auto Brightness');
const byName = n => d.getElementsByName(n);
const last = n => { const a=byName(n); return a.length?a[a.length-1]:null; };
const visible = el => { for(let n=el;n;n=n.parentElement){ if(n.style && n.style.display==='none') return false; } return true; };

ok(!!sec, 'Auto Brightness sec located');
// headers restyled
const heads = [...sec.querySelectorAll('.abrih')].map(p=>p.textContent);
ok(heads.includes('Light Sensor') && heads.includes('Brightness') && heads.includes('Off When Dark'),
   'group titles restyled to .abrih: '+JSON.stringify(heads));
ok(heads.includes('Live'), 'Live header present as .abrih');
// tables built, fields inside
const tbls = [...sec.querySelectorAll('table.abritbl')];
ok(tbls.length >= 6, `${tbls.length} .abritbl tables built (expect 6: sensor, cal, mapping, bri-settings, dark, mqtt + live readout = 7)`);
const cal = d.getElementById('abriCal');
ok(cal && cal.contains(last('Auto Brightness:Light Sensor:Cal Dark Raw')) && cal.contains(last('Auto Brightness:Light Sensor:Cal Bright Lux')),
   'calibration inputs moved into #abriCal');
ok(last('Auto Brightness:Brightness:Lux Min').closest('table.abritbl') != null, 'Lux Min inside a table');
ok(last('Auto Brightness:MQTT & Home Assistant:Home Assistant Discovery').closest('table.abritbl') != null, 'HA Discovery inside a table');
// Source/BH/Pin are selects now
ok(last('Auto Brightness:Light Sensor:Source').tagName==='SELECT', 'Source is a <select>');
ok(last('Auto Brightness:Light Sensor:Analog Pin').tagName==='SELECT', 'Analog Pin is a <select>');
const apOpts = [...last('Auto Brightness:Light Sensor:Analog Pin').options].map(o=>o.value);
ok(apOpts[0]==='-1' && apOpts.includes('34') && !apOpts.includes('21'), 'Analog Pin options: unused + ADC pins only ('+apOpts.join(',')+')');
// Analog Pin kept its saved value (34) selected
ok(last('Auto Brightness:Light Sensor:Analog Pin').value==='34', 'Analog Pin select preselects saved value 34');
// master relabel + hint on its own line
const enHidden = byName('Auto Brightness:Enabled')[0];
ok(enHidden.previousSibling && /Enable\b/.test(enHidden.previousSibling.textContent) && !/Enabled/.test(enHidden.previousSibling.textContent),
   'master label reads "Enable" (not "Enabled")');
ok(!!sec.querySelector('i.abrii'), 'hint rendered as .abrii');
// NO orphaned FIELD label text left loose in the sec (the v1.0.1 bug). The master
// "Enable" label legitimately stays inline (it isn't a table field); everything else
// (Source, BH1750 Address, Analog Pin, Smoothing, ...) must live inside a table cell.
const orphans = [...sec.childNodes].filter(n=>n.nodeType===3 && n.textContent.trim()!=='')
  .map(n=>n.textContent.trim()).filter(t=>t!=='Enable');
ok(orphans.length===0, 'no orphaned FIELD label text directly in sec: '+JSON.stringify(orphans));
// Reset Offset button sits in the Allow Manual Offset cell
const amo = last('Auto Brightness:Brightness:Allow Manual Offset');
ok(amo.closest('td') && [...amo.closest('td').querySelectorAll('button')].some(b=>b.textContent==='Reset Offset'),
   'Reset Offset button in the Allow Manual Offset cell');
// conditional visibility across all four sources
const sel = last('Auto Brightness:Light Sensor:Source');
const bhRow = last('Auto Brightness:Light Sensor:BH1750 Address').closest('tr');
const apRow = last('Auto Brightness:Light Sensor:Analog Pin').closest('tr');
const setSrc = v => { sel.value = v; sel.dispatchEvent(new w.Event('change')); };
ok(visible(bhRow) && !visible(apRow) && !visible(cal), 'Auto(0): BH row shown; analog pin + cal hidden');
setSrc('1'); ok(visible(bhRow) && !visible(apRow) && !visible(cal), 'BH1750(1): BH row shown; analog hidden');
setSrc('2'); ok(!visible(bhRow) && !visible(apRow) && !visible(cal), 'VEML(2): all conditional hidden');
setSrc('3'); ok(!visible(bhRow) && visible(apRow) && visible(cal), 'Analog(3): pin + cal shown; BH hidden');
// hidden != dropped: the field's real value still submits. WLED emits a hidden
// type-marker (value "number"/"text") BEFORE each field, so the real value is the
// 2nd same-named entry -> assert via getAll(), mirroring WLED's own POST parsing.
setSrc('1');
const fd = new w.FormData(d.querySelector('form'));
ok(fd.getAll('Auto Brightness:Light Sensor:Analog Pin').includes('34'), 'hidden Analog Pin value still submits (34)');
ok(fd.getAll('Auto Brightness:Light Sensor:Cal Dark Raw').includes('200'), 'hidden cal field value still submits (200)');
ok(fd.getAll('Auto Brightness:Enabled').includes('true'), 'checked Enabled still submits true');
// guard: empty DOM must not throw
try { const d2=new JSDOM('<body><div id="um"></div></body>',{runScripts:'outside-only'}); d2.window.d=d2.window.document;
  d2.window.addDropdown=()=>null; d2.window.addOption=()=>{}; d2.window.addInfo=()=>{}; d2.window.fetch=()=>Promise.reject();
  d2.window.eval(js); ok(true,'guard: no throw on a DOM without our fields'); }
catch(e){ ok(false,'guard: threw on empty DOM: '+e.message); }

console.log(fails? `\n${fails} FAILED` : '\nALL PASS');
process.exit(fails?1:0);
