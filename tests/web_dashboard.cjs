// Optional DOM integration check: npm install --prefix /tmp/p1scope-web-qa jsdom@26.1.0
// NODE_PATH=/tmp/p1scope-web-qa/node_modules node tests/web_dashboard.cjs
const {JSDOM}=require('jsdom');
const fs=require('node:fs');
const assert=require('node:assert/strict');
const now=1800000000;
const samples=Array.from({length:361},(_,i)=>({t:now-3600+i*10,grid:500+Math.sin(i)*100,
    l1:200,l2:100,l3:200,gas:100+Math.floor(i/30)*.01,breakBefore:i===180}));
const snapshot={now,started:now-3600,simulation:true,historySeconds:3600,
    measurement:{valid:true,requestFailed:false,updated:now,grid:500,phases:[200,100,200],
        voltage:[230,230,230],current:[1,1,1],gas:100.12,gasTimestamp:'October 21, 2026 20:40:00',tariff:'2'},
    gasSession:{used:.12,meterDecreased:false},groupsError:'',
    groups:[{number:1,name:'<img src=x onerror=alert(1)>',phase:1},{number:2,name:'Unknown',phase:null}],samples,
    hue:{status:'Hue: simulated temperatures',sensors:['Hal','Woonkamer','Badkamer'].map((name,i)=>({id:String(i),name,
        history:Array.from({length:121},(_,j)=>({t:now-3600+j*30,value:18+i+j/120}))}))}};
async function check(width){
    const original=structuredClone(snapshot);
    const dom=new JSDOM(fs.readFileSync('web/index.html','utf8'),{url:'http://localhost',runScripts:'outside-only'});
    const w=dom.window;
    const css=w.document.createElement("style");css.textContent=fs.readFileSync("web/dashboard.css","utf8");w.document.head.append(css);
    Object.defineProperty(w.HTMLElement.prototype,'clientWidth',{get(){return width;}});
    w.ResizeObserver=class{observe(){}};
    w.matchMedia=()=>({addEventListener(){}});
    let reloads=0,fail=false;
    w.fetch=async url=>{
        if(fail)throw Error('Offline');
        if(url.includes('reload')){reloads++;return {ok:true,json:async()=>({ok:true})};}
        return {ok:true,json:async()=>snapshot};
    };
    w.AbortSignal.timeout=()=>undefined;
    w.eval(fs.readFileSync('web/vendor/d3.min.js','utf8'));
    w.eval(fs.readFileSync('web/dashboard.js','utf8'));
    await new Promise(resolve=>setTimeout(resolve,30));
    const d=w.document;
    assert.match(d.querySelector('[data-status]').textContent,/SIMULATION/);
    assert.equal(d.querySelectorAll('[data-phases] .p-phase').length,3);
    assert.equal(d.querySelectorAll('[data-phases] img').length,0);
    assert.match(d.querySelector('[data-phases]').textContent,/<img/);
    assert.equal(d.querySelectorAll('[data-heat-legend] button').length,5);
    for(const path of d.querySelectorAll('path'))assert.ok(!/NaN|undefined/.test(path.getAttribute('d')||''));
    const trend=d.querySelector('[data-trend]');trend.checked=false;trend.dispatchEvent(new w.Event('change'));
    assert.equal(d.querySelectorAll('[data-heat-legend] button').length,4);
    const history=d.querySelector('select');history.value='5';history.dispatchEvent(new w.Event('change'));
    assert.equal(JSON.parse(w.localStorage.getItem('p1scope-view')).minutes,5);
    const legend=d.querySelector('[data-power-legend] button');legend.click();
    assert.equal(d.querySelector('[data-power-legend] button').getAttribute('aria-pressed'),'false');
    const overlay=d.querySelector('[data-heat] [data-chart-hit]');
    overlay.dispatchEvent(new w.MouseEvent('pointermove',{clientX:100,clientY:80}));
    assert.equal(d.querySelector('.p-tip').hidden,false);
    assert.match(d.querySelector('.p-tip').textContent,/Woonkamer/);
    const hybrid=d.querySelector('[data-hybrid]');hybrid.checked=false;hybrid.dispatchEvent(new w.Event('change'));
    assert.equal(d.querySelector('[data-hybrid-panel]').hidden,true);
    d.querySelector('[data-reload]').click();await new Promise(resolve=>setTimeout(resolve,30));
    assert.equal(reloads,1);
    // Empty readings and no Hue sensors must still render without NaN paths.
    snapshot.samples=[];snapshot.hue.sensors=[];snapshot.measurement.valid=false;
    hybrid.checked=true;hybrid.dispatchEvent(new w.Event('change'));
    assert.match(d.querySelector('[data-power]').textContent,/Waiting/);
    for(const path of d.querySelectorAll('path'))assert.ok(!/NaN|undefined/.test(path.getAttribute('d')||''));
    Object.assign(snapshot,original);
    dom.window.close();
}
(async()=>{await check(1024);await check(360);console.log('Dashboard DOM checks passed: rendering, missing data, safe labels, history, legends, toggles, hover, reload.');})().catch(error=>{console.error(error);process.exit(1);});
