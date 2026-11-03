const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {execFileSync} = require('node:child_process');
const page = fs.readFileSync(path.join(__dirname, '../bare/page.h'), 'utf8');
const script = page.match(/<script>([\s\S]*?)<\/script>/)[1];
execFileSync(process.execPath, ['--check'], {input: script});
assert(!/loadFiles|renderFiles|sendOnly|id="(?:stage|ftab|space|bprint)"/.test(page));
assert(!/['"]\/(?:api\/files['"]|select\?|delete\?|api\/rename)/.test(script));
assert(script.includes("x.open('POST', '/api/files/local')"));
class Element {
  constructor(tag='div') {
    this.tag=tag; this.parentElement={}; this.children=[]; this.dataset={}; this.style={}; this.className='';
    this.classList={add(){},remove(){},toggle(){}};
  }
  set textContent(text) { this.text=String(text);this.children=[]; }
  get textContent() { return (this.text || '')+this.children.map(c=>c.textContent).join(''); }
  append(...children) { this.children.push(...children); }
  insertRow() { const row=new Element('tr');this.append(row);return row; }
  insertCell() { const cell=new Element('td');this.append(cell);return cell; }
  get rows() { return this.children; }
  setAttribute() {}
  removeAttribute(name) { delete this[name]; }
  querySelectorAll(selector) {
    return this.children.flatMap(c=>[
      ...(selector[0]==='.' ? c.className.split(' ').includes(selector.slice(1)) : c.tag===selector) ? [c] : [],
      ...c.querySelectorAll(selector)
    ]);
  }
  querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
}
const elements=Object.fromEntries([...page.matchAll(/id="([^"]+)"/g)].map(m=>[m[1],new Element()]));
elements.bars.append(new Element('title'),new Element('rect'),new Element('rect'),new Element('rect'));
elements.sn.firstElementChild=new Element('polyline');
elements.sb.firstElementChild=new Element('polyline');
const requests=[];
let response={ok:true,status:200,text:async()=> 'BOATWI~1.GCO  123  Boat with spaces.gcode\n'};
const context=vm.createContext({
  document:{getElementById(id){assert(elements[id],id);return elements[id];},createElement:tag=>new Element(tag),documentElement:new Element()},
  localStorage:{},location:{hostname:'192.168.1.83'},URLSearchParams,Date,setTimeout(){},confirm:()=>true,
  fetch:async (url,opt)=>{requests.push([url,opt]);if(response instanceof Error) throw response;return response;}
});
vm.runInContext(script.slice(0,script.indexOf('(async function loop(){')),context);
const run=code=>vm.runInContext(code,context);
const status={system:{machine:'KP3S',rssi:-60,version:'test'},printing:true,connected:true,locked:true,state:'printing',
  job:{file:'Boat with spaces.gcode',completion:25,layer:2,layers:9,z:0.4,elapsed:60,remaining:-1,fan:-1},
  energy:{watt:-1,grams:0},socket:{token:false,device:false,auto:false,state:'disabled'},
  temperature:{tool:{actual:20,target:0},bed:{actual:20,target:0}}};
(async()=>{
  await run('printerCard()');
  assert.equal(elements.ptab.rows.length,1);
  assert(elements.ptab.textContent.includes('Boat with spaces.gcode'));
  assert.equal(elements.ptab.querySelector('img').src,'/thumbnail?name=Boat%20with%20spaces.gcode&s=123');
  await elements.ptab.querySelectorAll('button')[0].onclick();
  assert.equal(requests.at(-1)[0],'/printer/print?name=BOATWI~1.GCO');
  context.status=status;run('render(status)');
  assert.equal(elements.jlayer.textContent,'2 / 9');
  assert.equal(elements.jz.textContent,'0.40 mm');
  assert.equal(elements.jfan.textContent,'—');
  assert.equal(elements.jthumb.src,'/thumbnail?name=Boat%20with%20spaces.gcode');
  assert(elements.ptab.querySelectorAll('button').every(b=>b.disabled));
  for (const failure of [
    {ok:false,status:503,text:async()=> 'no cached printer list; refresh when idle'},
    new Error('network offline'),
    {ok:true,text:async()=> ''},
    {ok:true,text:async()=> 'NEW.GCO 1 new.gcode\nbroken listing\n'}
  ]) {
    response=failure;
    const rows=elements.ptab.rows;
    await run('printerCard()');
    assert.equal(elements.ptab.rows,rows);
    assert(elements.pnote.textContent.length);
  }
  response={ok:true,text:async()=> 'cached: last complete printer listing; refresh when idle\nBOATWI~1.GCO  123  Boat with spaces.gcode\n'};
  await run('printerCard()');
  assert(elements.pnote.textContent.startsWith('cached:'));
  assert(elements.ptab.querySelectorAll('button').every(b=>b.disabled));
  status.printing=false;run('render(status)');
  assert(elements.ptab.querySelectorAll('button').every(b=>!b.disabled));
  response={ok:true,text:async()=> "nothing on the printer's card\n"};
  await run('printerCard()');assert(elements.ptab.textContent.includes('nothing on'));
  const emptyRows=elements.ptab.rows;
  response=new Error('offline');await run('printerCard()');assert.equal(elements.ptab.rows,emptyRows);
  run('havePrinterList=false');await run('printerCard()');
  assert.equal(elements.ptab.rows.length,0);assert.equal(elements.pnote.textContent,'offline');
  status.job={file:'—',completion:0,layer:0,layers:0,z:0,elapsed:0,remaining:-1,fan:-1};
  run('render(status)');assert.equal(elements.jlayer.textContent,'—');assert.equal(elements.jthumb.src,undefined);
  assert(!requests.some(([url])=>url==='/api/files' || url.startsWith('/select')));
  console.log('PASS page: full JS syntax, DOS actions, printing/cache/error/empty states, unknown metadata, no ESP listing requests');
})().catch(e=>{console.error(e);process.exitCode=1;});
