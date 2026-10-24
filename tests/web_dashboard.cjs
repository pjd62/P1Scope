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
        voltage:[230,230,230],current:[1,1,1],gas:100.12,gasTimestamp:'October 21, 2026 20:40:00',tariff:'2',wifiSsid:'Home <WiFi>',wifiStrength:96},
    gasSession:{used:.12,meterDecreased:false},groupsError:'',
    groups:[{number:1,name:'<img src=x onerror=alert(1)>',phase:1},{number:2,name:'Unknown',phase:null}],samples,
    hue:{lightsAvailable:true,lightsStatus:'Simulated lights',lights:[{id:'a',name:'<img src=x>',room:'<Living room>',roomId:'living',on:true,brightness:75},{id:'b',name:'Hall',room:'Hall',roomId:'hall',on:false,brightness:null}],status:'Hue: simulated temperatures',sensors:['Hal','Woonkamer','Badkamer'].map((name,i)=>({id:String(i),name,
        history:Array.from({length:121},(_,j)=>({t:now-3600+j*30,value:18+i+j/120}))}))}};
async function check(width){
    const original=structuredClone(snapshot);
    const dom=new JSDOM(fs.readFileSync('web/index.html','utf8'),{url:'http://localhost',runScripts:'outside-only'});
    const w=dom.window;
    const css=w.document.createElement("style");css.textContent=fs.readFileSync("web/dashboard.css","utf8");w.document.head.append(css);
    Object.defineProperty(w.HTMLElement.prototype,'clientWidth',{get(){return width;}});
    w.HTMLElement.prototype.getBoundingClientRect=function(){
        const power=w.document.querySelector('[data-power]');
        const powerHeight=+(power?.getAttribute('height')||260);
        const height=this.id==='p1scope-web'?250+powerHeight+
            (w.document.querySelector('[data-hybrid-panel]').hidden?0:320)+
            (w.document.querySelector('[data-phases]').hidden?0:220):0;
        return {top:0,left:0,width,height,right:width,bottom:height};
    };
    w.SVGElement.prototype.getBoundingClientRect=function(){return {top:0,left:0,width,height:+(this.getAttribute('height')||0)};};
    w.HTMLDialogElement.prototype.showModal=function(){this.open=true;};
    w.HTMLDialogElement.prototype.close=function(){this.open=false;};
    w.ResizeObserver=class{observe(){}};
    w.matchMedia=()=>({addEventListener(){}});
    let reloads=0,fail=false,lightRequests=0,rejectLight=false;
    w.fetch=async url=>{
        if(fail)throw Error('Offline');
        if(url.includes('/api/hue/lights/')) {
            lightRequests++;
            if(rejectLight)return {ok:true,json:async()=>({ok:false,error:'Bridge rejected change'})};
            const parts=url.split('/'),light=snapshot.hue.lights.find(item=>item.id===parts.at(-2));
            if(parts.at(-1).startsWith('brightness-'))light.brightness=+parts.at(-1).slice(11);
            else light.on=parts.at(-1)==='on';
            return {ok:true,json:async()=>({ok:true})};
        }
        if(url.includes('reload')){reloads++;return {ok:true,json:async()=>({ok:true})};}
        return {ok:true,json:async()=>snapshot};
    };
    w.AbortSignal.timeout=()=>undefined;
    w.eval(fs.readFileSync('web/vendor/d3.min.js','utf8'));
    w.eval(fs.readFileSync('web/dashboard.js','utf8'));
    await new Promise(resolve=>setTimeout(resolve,30));
    const d=w.document;
    d.querySelector('[data-lights-open]').click();
    assert.equal(d.querySelector('.p-lights').open,true);
    assert.equal(d.querySelectorAll('[data-lights-list] tr:not(.p-light-room)').length,2);
    assert.equal(d.querySelectorAll('[data-lights-list] img').length,0);
    assert.deepEqual([...d.querySelectorAll('.p-light-room th')].map(el=>el.textContent),['<Living room>','Hall']);
    assert.match(d.querySelector('[data-lights-list]').textContent,/75%/);
    assert.match(d.querySelector('[data-lights-list]').textContent,/HallTurn onUnavailable/);
    d.querySelector('[data-lights-list] button').click();
    await new Promise(resolve=>setTimeout(resolve,30));
    assert.equal(lightRequests,1);
    assert.equal(snapshot.hue.lights[0].on,false);
    assert.match(d.querySelector('[data-lights-list] button').textContent,/Turn on/);
    const slider=d.querySelector('[data-lights-list] input[type=range]');
    slider.value='35';slider.dispatchEvent(new w.Event('input'));slider.dispatchEvent(new w.Event('change'));
    await new Promise(resolve=>setTimeout(resolve,30));
    assert.equal(snapshot.hue.lights[0].brightness,35);
    assert.equal(snapshot.hue.lights[0].on,false);
    snapshot.hue.lights[1].reachable=false;
    d.querySelector('[data-reference]').dispatchEvent(new w.Event('change'));
    assert.equal(d.querySelectorAll('[data-lights-list] button').length,1);
    const unavailable=d.querySelector('[data-show-unavailable]');unavailable.checked=true;unavailable.dispatchEvent(new w.Event('change'));
    assert.equal(d.querySelectorAll('[data-lights-list] button').length,2);
    assert.equal(d.querySelectorAll('[data-lights-list] button')[1].disabled,true);
    assert.match(d.querySelector('[data-lights-list]').textContent,/Hall \(unavailable\)/);
    snapshot.hue.lights[1].reachable=true;
    rejectLight=true;
    d.querySelector('[data-lights-list] button').click();
    await new Promise(resolve=>setTimeout(resolve,30));
    assert.match(d.querySelector('[data-lights-action]').textContent,/Bridge rejected/);
    assert.equal(snapshot.hue.lights[0].on,false);
    snapshot.hue.pendingLight='a';
    d.querySelector('[data-reference]').dispatchEvent(new w.Event('change'));
    assert.ok([...d.querySelectorAll('[data-lights-list] button')].every(button=>button.disabled));
    snapshot.hue.pendingLight='';
    snapshot.hue.lightsAvailable=false;
    d.querySelector('[data-reference]').dispatchEvent(new w.Event('change'));
    assert.match(d.querySelector('[data-lights-status]').textContent,/Last known/);
    assert.doesNotMatch(d.querySelector('[data-lights-list]').textContent,/75%/);
    snapshot.hue.lightsAvailable=true;snapshot.hue.lights=[];
    d.querySelector('[data-reference]').dispatchEvent(new w.Event('change'));
    assert.match(d.querySelector('[data-lights-count]').textContent,/No lights/);
    snapshot.hue=structuredClone(original.hue);
    d.querySelector('[data-lights-close]').click();
    assert.equal(d.querySelector('.p-lights').open,false);
    assert.match(d.querySelector('[data-status]').textContent,/SIMULATION/);
    assert.match(d.querySelector('[data-session-label]').textContent,/\(1h 00m 00s\): /);
    assert.match(d.querySelector('[data-session]').textContent,/0.120 m³ \(first to latest received reading\)/);
    assert.match(d.querySelector('[data-p1-status]').textContent,/Wi-Fi: Home <WiFi> \| 96%/);
    assert.equal(d.querySelector('[data-grid-value]').closest('section'),d.querySelector('[data-power]').closest('section'));
    assert.equal(d.querySelector('[data-session]').closest('section'),d.querySelector('[data-heat]').closest('section'));
    assert.ok(d.querySelector('[data-power]').compareDocumentPosition(d.querySelector('[data-grid-value]')) & w.Node.DOCUMENT_POSITION_FOLLOWING);
    assert.ok(d.querySelector('[data-heat]').compareDocumentPosition(d.querySelector('[data-session]')) & w.Node.DOCUMENT_POSITION_FOLLOWING);
    assert.equal(d.querySelectorAll('[data-phases] .p-phase').length,3);
    const phaseCards=d.querySelector('[data-phase-cards]');
    phaseCards.checked=false;phaseCards.dispatchEvent(new w.Event('change'));
    assert.equal(d.querySelector('[data-phases]').hidden,true);
    assert.equal(JSON.parse(w.localStorage.getItem('p1scope-view'))['phase-cards'],false);
    phaseCards.checked=true;phaseCards.dispatchEvent(new w.Event('change'));
    assert.equal(d.querySelector('[data-phases]').hidden,false);
    assert.equal(d.querySelectorAll('[data-phases] img').length,0);
    assert.match(d.querySelector('[data-phases]').textContent,/<img/);
    assert.equal(d.querySelectorAll('[data-heat-legend] button').length,5);
    for(const path of d.querySelectorAll('path'))assert.ok(!/NaN|undefined/.test(path.getAttribute('d')||''));
    const trend=d.querySelectorAll('[data-heat-legend] button')[1];trend.click();
    assert.equal(d.querySelectorAll('[data-heat-legend] button').length,5);
    assert.equal(d.querySelectorAll('[data-heat-legend] button')[1].getAttribute('aria-pressed'),'false');
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
    const gasHiddenHeight=+d.querySelector('[data-power]').getAttribute('height');
    phaseCards.checked=false;phaseCards.dispatchEvent(new w.Event('change'));
    const expandedHeight=+d.querySelector('[data-power]').getAttribute('height');
    assert.ok(expandedHeight>gasHiddenHeight);
    assert.ok(d.querySelector('[data-power]').getAttribute('viewBox').endsWith(' '+expandedHeight));
    Object.defineProperty(w,'innerHeight',{value:w.innerHeight+200,configurable:true});
    w.dispatchEvent(new w.Event('resize'));
    assert.equal(+d.querySelector('[data-power]').getAttribute('height'),expandedHeight+200);
    phaseCards.checked=true;phaseCards.dispatchEvent(new w.Event('change'));

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
