'use strict';
(() => {
const root = document.getElementById('p1scope-web');
const $ = selector => root.querySelector(selector);
let snapshot = null;
let stored = {};
try { stored = JSON.parse(localStorage.getItem('p1scope-view') || '{}'); } catch (_) {}
const state = {minutes: [1,5,10,30,60,360,1440].includes(stored.minutes) ? stored.minutes : 60,
    reference: !!stored.reference, hybrid: stored.hybrid !== false, 'phase-cards': stored['phase-cards'] !== false,
    hidden: new Set(Array.isArray(stored.hidden) ? stored.hidden : [])};
const colors = {grid:'--p-gridline', l1:'--p-l1', l2:'--p-l2', l3:'--p-l3', gas:'--p-gas', trend:'--p-trend'};
const palette = ['--p-l2','--p-l1','--p-l3','--p-gas'];
const tip = $('.p-tip');
const finite = value => typeof value === 'number' && Number.isFinite(value);
const number = (value, digits=0) => finite(value) ? value.toLocaleString(undefined,{minimumFractionDigits:digits,maximumFractionDigits:digits}) : 'unavailable';
const date = seconds => new Date(seconds * 1000);
const clock = seconds => d3.timeFormat('%H:%M:%S')(date(seconds));
function persist() { try { localStorage.setItem('p1scope-view',JSON.stringify({...state,hidden:[...state.hidden]})); } catch (_) {} }
function node(tag, text, className) { const el = document.createElement(tag); if (text != null) el.textContent=text; if(className)el.className=className;return el; }
function lineBreaks(rows) {
    const result=[];
    for(const row of rows) {
        if(row.breakBefore && result.length) result.push({t:row.t-0.001,grid:null,l1:null,l2:null,l3:null,gas:null});
        result.push(row);
    }
    return result;
}
function nearest(history, time) {
    if(!history.length || time<history[0].t || time>history.at(-1).t) return null;
    const right=d3.bisector(p=>p.t).left(history,time);
    const a=history[Math.max(0,right-1)], b=history[Math.min(history.length-1,right)];
    if(time!==b.t && (!finite(a.value)||!finite(b.value)))return null;
    return Math.abs(a.t-time)<Math.abs(b.t-time)?a:b;
}
function legend(selector, series) {
    const box=$(selector);box.replaceChildren();
    for(const seriesItem of series) {
        const button=node('button',null,'p-series');button.type='button';
        button.setAttribute('aria-pressed',String(!state.hidden.has(seriesItem.id)));
        const swatch=node('span',null,'p-swatch'+(seriesItem.id==='trend'?' p-dot':''));
        swatch.style.setProperty('--series-color','var('+seriesItem.color+')');
        button.append(swatch,document.createTextNode(seriesItem.name));
        button.addEventListener('click',()=>{state.hidden.has(seriesItem.id)?state.hidden.delete(seriesItem.id):state.hidden.add(seriesItem.id);persist();render();});
        box.append(button);
    }
}
const logsDialog = $('.p-logs'), logsOutput = $('[data-logs-output]');
let logsTimer, logsLoading = false, logsFirst = true;
async function pollLogs() {
    if (!logsDialog.open || logsLoading) return;
    logsLoading = true;
    try {
        const response = await fetch('/api/logs', {cache:'no-store', signal:AbortSignal.timeout(8000)});
        if (!response.ok) throw Error('HTTP ' + response.status);
        const text = await response.text();
        const atBottom = logsFirst || logsOutput.scrollTop + logsOutput.clientHeight >= logsOutput.scrollHeight - 24;
        logsOutput.textContent = text || 'No diagnostics yet.';
        if (atBottom) logsOutput.scrollTop = logsOutput.scrollHeight;
        logsFirst = false;
        $('[data-logs-status]').textContent = 'Live · Refreshing every 2 seconds';
    } catch (_) {
        $('[data-logs-status]').textContent = 'Could not refresh logs. Retrying…';
    } finally {
        logsLoading = false;
        clearTimeout(logsTimer);
        if (logsDialog.open) logsTimer = setTimeout(pollLogs, 2000);
    }
}
$('[data-logs-open]').addEventListener('click', () => {
    logsDialog.showModal();
    logsFirst = true;
    $('[data-logs-status]').textContent = 'Loading…';
    pollLogs();
});
$('[data-logs-close]').addEventListener('click', () => logsDialog.close());
logsDialog.addEventListener('close', () => clearTimeout(logsTimer));
const lightsDialog = $('.p-lights');
$('[data-lights-open]').addEventListener('click', () => lightsDialog.showModal());
$('[data-lights-close]').addEventListener('click', () => lightsDialog.close());
let lightSending = false, lightError = '';
$('[data-show-unavailable]').addEventListener('change', renderLights);
function renderLights() {
    const hue = snapshot.hue, lights = hue.lights || [];
    $('[data-lights-status]').textContent = (hue.lightsStatus || hue.status) +
        (!hue.lightsAvailable && lights.length ? ' · Last known lights; states may be out of date.' : '');
    $('[data-lights-count]').textContent = hue.lightsAvailable && !lights.length ?
        'No lights found on this bridge.' : lights.length + ' light(s), ' + lights.filter(light => light.reachable === false).length + ' unavailable';
    $('[data-lights-action]').textContent = lightError || hue.lightActionStatus || '';
    const list = $('[data-lights-list]');
    if (list.contains(document.activeElement) && document.activeElement.type === 'range' && !lightSending) return;
    list.replaceChildren();
    let previousRoom;
    for (const light of lights) {
        if (light.reachable === false && !$('[data-show-unavailable]').checked) continue;
        const roomId = light.roomId || '';
        if (previousRoom !== roomId) {
            const heading = node('tr', null, 'p-light-room'), title = node('th', light.room || 'Unassigned');
            title.colSpan = 3; title.scope = 'rowgroup'; heading.append(title); list.append(heading);
            previousRoom = roomId;
        }
        const row = node('tr');
        row.append(node('td', light.name + (light.reachable === false ? ' (unavailable)' : '')));
        const control = node('td'), button = node('button', light.on ? 'Turn off' : 'Turn on', 'p-series');
        button.type = 'button';
        button.classList.add('p-light-toggle');
        button.dataset.on = String(light.on);
        button.setAttribute('aria-label', (light.on ? 'Turn off ' : 'Turn on ') + light.name);
        button.disabled = lightSending || !!hue.pendingLight || !hue.lightsAvailable || light.reachable === false;
        const sendChange = async action => {
            lightSending = true; lightError = ''; renderLights();
            try {
                const response = await fetch('/api/hue/lights/' + encodeURIComponent(light.id) + '/' + action,
                    {method:'POST', headers:{'X-P1Scope-Action':'light'}, signal:AbortSignal.timeout(8000)});
                if (!response.ok) throw Error('Light change request failed');
                const result = await response.json();
                if (!result.ok) throw Error(result.error || 'Light change rejected');
                await poll();
            } catch (error) { lightError = error.message || 'Could not change light'; }
            finally { lightSending = false; renderLights(); }
        };
        button.addEventListener('click', () => sendChange(light.on ? 'off' : 'on'));
        control.append(button); row.append(control);
        const brightness = node('td');
        if (finite(light.brightness)) {
            const slider = node('input'), label = node('output', number(light.brightness) + '%');
            slider.type = 'range'; slider.min = '0'; slider.max = '100'; slider.step = '1'; slider.value = Math.round(light.brightness);
            slider.setAttribute('aria-label', 'Brightness for ' + light.name);
            slider.disabled = button.disabled;
            slider.addEventListener('input', () => { label.textContent = slider.value + '%'; });
            slider.addEventListener('change', () => sendChange('brightness-' + slider.value));
            brightness.append(slider, label);
        } else brightness.textContent = 'Unavailable';
        row.append(brightness); list.append(row);
    }
}
function render() {
    if(!snapshot)return;
    renderLights();
    const m=snapshot.measurement,sim=snapshot.simulation?' (simulated)':'';
    $('select').value=state.minutes;
    for(const key of ['reference','hybrid','phase-cards'])$('[data-'+key+']').checked=state[key];
    $('[data-hybrid-panel]').hidden=!state.hybrid;
    $('[data-phases]').hidden=!state['phase-cards'];
    $('[data-status]').textContent=snapshot.simulation?'SIMULATION · Generated readings':'LIVE · Local network';
    $('[data-grid-label]').textContent='Grid '+(m.grid>=0?'import':'export')+sim;
    $('[data-grid-value]').textContent=m.valid?number(Math.abs(m.grid))+' W':'Waiting…';
    $('[data-tariff]').textContent=m.valid?'Tariff '+m.tariff:'';
    const elapsed=Math.max(0,Math.floor(snapshot.now-snapshot.started));
    $('[data-session-label]').textContent='Gas used since startup'+sim+' ('+Math.floor(elapsed/3600)+'h '+String(Math.floor(elapsed/60)%60).padStart(2,'0')+'m '+String(elapsed%60).padStart(2,'0')+'s): ';
    $('[data-session]').textContent=snapshot.gasSession.meterDecreased?'unavailable (meter reading decreased)':
        finite(snapshot.gasSession.used)?number(snapshot.gasSession.used,3)+' m³ (first to latest received reading)':'waiting for first gas reading';
    $('[data-gas-meter]').textContent='Gas meter'+sim+': '+number(m.gas,3)+' m³ · Updated: '+(m.gasTimestamp||'unavailable');
    const stale=m.valid&&(m.requestFailed||snapshot.now-m.updated>3);
    const connection=!m.valid?'P1: waiting for readings':stale?'P1: stale — '+Math.floor(snapshot.now-m.updated)+' s since last reading':'P1: '+(snapshot.simulation?'simulated readings':'connected');
    const wifi='Wi-Fi: '+(m.wifiSsid || 'unavailable')+' | '+(finite(m.wifiStrength)&&m.wifiStrength>=0?number(m.wifiStrength)+'%':'unavailable');
    $('[data-p1-status]').textContent=m.valid&&!stale?wifi:connection+' | '+wifi;
    if(snapshot.groupsError)$('[data-error]').textContent='Groups: '+snapshot.groupsError;
    const sensors=snapshot.hue.sensors;
    const readouts=$('[data-temperatures]');readouts.replaceChildren();
    sensors.forEach((sensor,i)=>{
        const latest=sensor.history.at(-1);const label=node('span',sensor.name+': '+number(latest?.value,2)+(finite(latest?.value)?' °C':''));
        label.style.color='var('+palette[i%palette.length]+')';readouts.append(label);
    });
    const phaseBox=$('[data-phases]');phaseBox.replaceChildren();
    for(let i=0;i<3;i++) {
        const section=node('section',null,'p-phase');section.style.setProperty('--phase-color','var('+colors['l'+(i+1)]+')');
        section.append(node('h2','L'+(i+1)),node('div',m.valid?number(m.phases[i])+' W':'Waiting…','p-value'),node('div',m.valid?number(m.voltage[i],1)+' V · '+(m.phases[i]>=0?'Import':'Export'):'No readings','p-sub'));
        const current=m.current[i],track=node('div',null,'p-track'),bar=node('span');bar.style.width=(finite(current)?Math.min(100,Math.abs(current)/25*100):0)+'%';track.append(bar);
        section.append(track,node('div',finite(current)?number(Math.abs(current),1)+' / 25 A':'Current unavailable','p-sub'));
        const groups=node('ul',null,'p-groups');snapshot.groups.filter(g=>g.phase===i+1).forEach(g=>groups.append(node('li',g.number+' · '+g.name)));
        if(!groups.children.length)groups.append(node('li','No groups assigned'));section.append(groups);phaseBox.append(section);
    }
    const unassigned=snapshot.groups.filter(g=>g.phase===null);
    if(unassigned.length){const text=node('div','Unassigned groups: '+unassigned.map(g=>g.number+' · '+g.name).join(' | '),'p-sub');text.style.gridColumn='1 / -1';phaseBox.append(text);}
    const start=snapshot.now-state.minutes*60,end=snapshot.now;
    const data=lineBreaks(snapshot.samples).filter(p=>p.t>=start&&p.t<=end);
    let baseline=null,lastGas=null,reset=false;
    const gasRows=data.map(row=>{
        if(!finite(row.gas))return {...row,usage:null};
        if(baseline===null)baseline=row.gas;
        if(lastGas!==null&&row.gas<lastGas)reset=true;
        lastGas=row.gas;return {...row,usage:reset?null:row.gas-baseline};
    });
    const valid=gasRows.filter(p=>finite(p.usage));
    const meanT=d3.mean(valid,p=>p.t-start)||0,meanG=d3.mean(valid,p=>p.usage)||0;
    const variance=d3.sum(valid,p=>(p.t-start-meanT)**2);
    const slope=valid.length>=2&&variance>0?d3.sum(valid,p=>(p.t-start-meanT)*(p.usage-meanG))/variance:null;
    const trend=t=>finite(slope)?meanG+slope*(t-start-meanT):null;
    const powerSeries=[['grid','Grid'],['l1','L1'],['l2','L2'],['l3','L3']].map(([id,name])=>({id,name,color:colors[id]}));
    const heatSeries=[{id:'gas',name:'Gas'+sim,color:colors.gas},{id:'trend',name:'Gas trend'+sim,color:colors.trend},...sensors.map((sensor,i)=>({id:'temp-'+sensor.id,name:sensor.name,color:palette[i%palette.length],sensor}))];
    legend('[data-power-legend]',powerSeries);legend('[data-heat-legend]',heatSeries);
    if(state.hybrid)draw('[data-heat]','heat',heatSeries);
    draw('[data-power]','power',powerSeries);
    function draw(selector,kind,series) {
        const element=$(selector),width=Math.max(250,element.parentElement.clientWidth-2*(parseFloat(getComputedStyle(element.parentElement).paddingLeft)||0));
        // Measure everything outside this chart after applying section visibility.
        // Subtracting its current height keeps repeated renders from growing the page.
        const viewportHeight=window.visualViewport?.height || window.innerHeight;
        const rootRect=root.getBoundingClientRect();
        const otherHeight=rootRect.height-element.getBoundingClientRect().height;
        const availableHeight=viewportHeight-(rootRect.top+window.scrollY)-otherHeight;
        const height=kind==='power'?Math.max(260,Math.floor(availableHeight)):270,margin={left:width<500?48:62,right:kind==='heat'?45:16,top:28,bottom:44};
        const x0=margin.left,x1=width-margin.right,y0=margin.top,y1=height-margin.bottom;
        const svg=d3.select(element).attr('viewBox','0 0 '+width+' '+height).attr('height',height);svg.selectAll('*').remove();
        svg.append('title').text(kind==='power'?'Grid power':'Gas usage and room temperatures');
        const x=d3.scaleLinear().domain([start,end]).range([x0,x1]);
        const active=series.filter(s=>!state.hidden.has(s.id));
        const powerMax=d3.max(data.flatMap(p=>active.map(s=>finite(p[s.id])?Math.abs(p[s.id]):0)))||0;
        const gasMax=d3.max(gasRows,p=>Math.max(p.usage||0,!state.hidden.has('trend')?(trend(p.t)||0):0))||0;
        const max=kind==='power'?Math.max(500,powerMax,state.reference?5750:0)*1.12:Math.max(.005,gasMax)*1.15;
        const y=d3.scaleLinear().domain(kind==='power'?[-max,max]:[0,max]).nice().range([y1,y0]);
        const temperatures=sensors.flatMap(s=>s.history.filter(p=>p.t>=start&&p.t<=end&&finite(p.value)).map(p=>p.value));
        const extent=d3.extent(temperatures);const yt=d3.scaleLinear().domain(temperatures.length?[Math.floor(extent[0]-.5),Math.ceil(extent[1]+.5)]:[15,25]).range([y1,y0]);
        const clip='p1scope-'+kind+'-clip';svg.append('defs').append('clipPath').attr('id',clip).append('rect').attr('x',x0).attr('y',y0).attr('width',x1-x0).attr('height',y1-y0);
        svg.append('rect').attr('data-chart-frame','').attr('x',x0).attr('y',y0).attr('width',x1-x0).attr('height',y1-y0).attr('fill','none').attr('stroke','var(--p-grid)');
        svg.append('g').attr('transform','translate('+x0+',0)').call(d3.axisLeft(y).ticks(4).tickSize(-(x1-x0)).tickFormat(kind==='power'?d3.format('~s'):d3.format('.3f'))).call(g=>g.select('.domain').remove());
        svg.append('g').attr('transform','translate(0,'+y1+')').call(d3.axisBottom(x).ticks(width<500?3:6).tickFormat(t=>d3.timeFormat('%H:%M')(date(t))));
        if(kind==='heat')svg.append('g').attr('transform','translate('+x1+',0)').call(d3.axisRight(yt).ticks(4).tickFormat(d3.format('.0f')));
        svg.append('text').attr('x',x0).attr('y',14).text(kind==='power'?'Power · W':'Gas · m³');
        svg.append('text').attr('x',(x0+x1)/2).attr('y',height-6).attr('text-anchor','middle').text('Time received');
        if(kind==='heat')svg.append('text').attr('x',x1).attr('y',14).attr('text-anchor','end').text('Temperature · °C');
        const lines=svg.append('g').attr('clip-path','url(#'+clip+')');
        if(kind==='power'&&state.reference)for(const ref of [-5750,5750])lines.append('line').attr('x1',x0).attr('x2',x1).attr('y1',y(ref)).attr('y2',y(ref)).attr('stroke','var(--p-muted)').attr('stroke-dasharray','5 5');
        for(const item of active) {
            const rows=item.sensor?item.sensor.history:kind==='power'?data:gasRows;
            const value=p=>item.sensor?p.value:item.id==='gas'?p.usage:item.id==='trend'?(finite(p.usage)?trend(p.t):null):p[item.id];
            const scale=item.sensor?yt:y;
            const line=d3.line().defined(p=>finite(value(p))).x(p=>x(p.t)).y(p=>scale(value(p))).curve(item.id==='gas'?d3.curveStepAfter:d3.curveLinear);
            const stroke=getComputedStyle(root).getPropertyValue(item.color).trim();
            lines.append('path').datum(rows).attr('d',line).attr('fill','none').attr('stroke',stroke).attr('stroke-width',item.id==='grid'?2:1.7).attr('stroke-dasharray',item.id==='trend'?'1 6':null).attr('stroke-linecap','round');
            // Keep the first reading visible before enough points exist for a line.
            if(rows.length===1&&finite(value(rows[0])))lines.append('circle').attr('cx',x(rows[0].t)).attr('cy',scale(value(rows[0]))).attr('r',2).attr('fill',stroke);
        }
        if(!data.length&&kind==='power')svg.append('text').attr('x',(x0+x1)/2).attr('y',(y0+y1)/2).attr('text-anchor','middle').text('Waiting for meter readings…');
        const guide=svg.append('line').attr('class','p-guide').attr('y1',y0).attr('y2',y1).style('display','none');const markers=svg.append('g');let pinned=false;
        function show(event) {
            const [pixel]=d3.pointer(event,element),time=x.invert(Math.max(x0,Math.min(x1,pixel)));
            guide.attr('x1',x(time)).attr('x2',x(time)).style('display',null);markers.selectAll('*').remove();tip.replaceChildren(node('strong',clock(time)+(snapshot.simulation?' · simulated':'')));
            for(const item of active) {
                let value=null,sampleTime=null;
                if(item.sensor){const point=nearest(item.sensor.history,time);if(point){value=point.value;sampleTime=point.t;}}
                else {
                    const rows=kind==='power'?data:gasRows;
                    if(rows.length&&time>=rows[0].t&&time<=rows.at(-1).t) {
                        const after=d3.bisector(p=>p.t).left(rows,time),a=rows[Math.max(0,after-1)],b=rows[Math.min(rows.length-1,after)];
                        if(item.id==='gas') value=time===b.t?b.usage:finite(a.usage)&&finite(b.usage)?a.usage:null;
                        else if(item.id==='trend')value=finite(a.usage)&&finite(b.usage)?trend(time):null;
                        else if(finite(a[item.id])&&finite(b[item.id]))value=a[item.id]+(b.t===a.t?0:(time-a.t)/(b.t-a.t))*(b[item.id]-a[item.id]);
                    }
                }
                const row=node('div'),label=node('span',item.name),valueText=node('span',finite(value)?number(value,kind==='power'?0:item.sensor?2:3)+' '+(kind==='power'?'W':item.sensor?'°C':'m³'):'No reading');
                label.style.color='var('+item.color+')';row.append(label,valueText);tip.append(row);
                if(finite(value))markers.append('circle').attr('cx',x(time)).attr('cy',(item.sensor?yt:y)(value)).attr('r',3).attr('fill',getComputedStyle(root).getPropertyValue(item.color).trim());
                if(sampleTime!==null)tip.append(node('small','Received '+clock(sampleTime)));
            }
            if(kind==='heat'&&!state.hidden.has('trend')&&finite(slope)){const row=node('div');row.append(node('span','Trend estimate'),node('span',number(slope*3600,3)+' m³/h'));tip.append(row);}
            tip.hidden=false;const r=root.getBoundingClientRect(),sr=element.getBoundingClientRect();
            tip.style.left=Math.max(8,Math.min(root.clientWidth-tip.offsetWidth-8,sr.left-r.left+x(time)+12))+'px';tip.style.top=Math.max(8,sr.top-r.top+y0+8)+'px';
        }
        svg.append('rect').attr('data-chart-hit','').attr('x',x0).attr('y',y0).attr('width',x1-x0).attr('height',y1-y0).attr('fill','transparent').on('pointermove',event=>{if(!pinned)show(event);}).on('pointerleave',()=>{if(!pinned){tip.hidden=true;guide.style('display','none');markers.selectAll('*').remove();}}).on('click',event=>{pinned=!pinned;show(event);});
    }
    tip.hidden=true;
}
$('select').addEventListener('change',event=>{state.minutes=+event.target.value;persist();render();poll();});
for(const key of ['reference','hybrid','phase-cards'])$('[data-'+key+']').addEventListener('change',event=>{state[key]=event.target.checked;persist();render();});
$('[data-reload]').addEventListener('click',async()=>{
    try { const response=await fetch('/api/groups/reload',{method:'POST',headers:{'X-P1Scope-Action':'reload'}});if(!response.ok)throw Error('Reload failed');const result=await response.json();$('[data-error]').textContent=result.ok?'Groups reloaded':result.error;await poll(); }catch(_){$('[data-error]').textContent='Could not reload groups';}
});
async function poll(){
    try { const response=await fetch('/api/state?minutes='+state.minutes,{cache:'no-store',signal:AbortSignal.timeout(8000)});if(!response.ok)throw Error('HTTP '+response.status);snapshot=await response.json();$('[data-error]').textContent='';render(); }
    catch(_){$('[data-status]').textContent='DISCONNECTED';$('[data-error]').textContent='Server unreachable. Displayed readings are stale; retrying…';}
    finally{clearTimeout(poll.timer);poll.timer=setTimeout(poll,2000);}
}
let previousWidth=0;new ResizeObserver(entries=>{const width=entries[0].contentRect.width;if(Math.abs(width-previousWidth)>1){previousWidth=width;render();}}).observe(root);
window.addEventListener('resize',render);
window.visualViewport?.addEventListener('resize',render);
matchMedia('(prefers-color-scheme: dark)').addEventListener('change',render);
if(typeof d3==='undefined')$('[data-error]').textContent='Chart library missing. Check web/vendor/d3.min.js on the server.';else poll();
})();
