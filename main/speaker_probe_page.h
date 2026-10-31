#pragma once
static constexpr const char kSpeakerProbePage[] = R"HTML(<!doctype html>
<html lang="nl"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bose-niveau · Johan2</title>
<style>
:root{color-scheme:dark;font:17px system-ui;background:#111b20;color:#eaf1ec}body{max-width:620px;margin:7vh auto;padding:24px}h1{font-size:2rem;letter-spacing:-.04em}p{line-height:1.6;color:#b7c8c3}.card{background:#1c2a2e;border:1px solid #31474a;border-radius:20px;padding:28px;margin:24px 0}.value{font-size:3rem;letter-spacing:-.04em;font-variant-numeric:tabular-nums}button{font:inherit;font-weight:600;border:0;border-radius:12px;background:#b9e6cd;color:#133126;padding:15px 22px;margin:8px 8px 8px 0;cursor:pointer}button.secondary{background:#31474a;color:#eaf1ec}button:disabled{opacity:.4;cursor:default}small{color:#b7c8c3}#status{min-height:3em}a{color:#b9e6cd}
</style>
<small>JOHAN2 · AKOESTISCHE METING</small><h1>Hoe hard staat de Bose?</h1>
<p>Meet met drie korte lage tonen. Muziek wordt ongeveer acht seconden gedempt en gaat daarna verder. Zeg tijdens de meting niets.</p>
<div class="card"><small>NIVEAU TEN OPZICHTE VAN JE REFERENTIE</small><div class="value" id="value">—</div><p id="detail">Meet eerst de huidige stand en bewaar die als referentie.</p><small id="baseline"></small></div>
<button id="measure">Meet Bose</button><button class="secondary" id="save" disabled>Deze stand als nul</button>
<p id="status" role="status" aria-live="polite">Klaar om te meten.</p>
<p><small>De testtonen klinken ook als de speler gedempt is. Houd de plek van speaker en microfoon gelijk: dit is een relatieve meting van het lage-tonengeluid bij de microfoon, geen uitlezing van de Bose-knop of gekalibreerde geluidsmeter.</small></p>
<script>
const el=id=>document.getElementById(id);let last=null,base=null;
try{const stored=JSON.parse(localStorage.getItem('johan2-bose-reference-v2'));if(stored&&Array.isArray(stored.bands)&&stored.bands.length===3)base=stored}catch{}
const median=a=>{a.sort((x,y)=>x-y);return a.length%2?a[a.length>>1]:(a[a.length/2-1]+a[a.length/2])/2};
function show(){
 el('baseline').textContent=base?'Referentie opgeslagen '+new Date(base.time).toLocaleString('nl-NL'):'';
 if(!last)return;
 const common=base?last.bands.flatMap((b,i)=>b.valid&&base.bands[i]?.valid&&b.hz===base.bands[i].hz?[b.gain_db-base.bands[i].gain_db]:[]):[];
 if(common.length>=2){const delta=median(common);el('value').textContent=(delta>=0?'+':'')+delta.toFixed(1)+' dB';el('detail').textContent=Math.abs(delta)<1?'Ongeveer dezelfde stand als je referentie.':delta>0?'Meer laag geluid dan bij je referentie.':'Minder laag geluid dan bij je referentie.'}
 else{el('value').textContent='—';el('detail').textContent=base?'Onvoldoende overeenkomstige tonen; probeer opnieuw.':'Meting geslaagd. Bewaar deze stand als referentie.'}
}
el('save').onclick=()=>{if(!last?.valid)return;base={time:Date.now(),bands:last.bands};try{localStorage.setItem('johan2-bose-reference-v2',JSON.stringify(base))}catch{el('status').textContent='De referentie werkt nu, maar kon niet in deze browser worden bewaard.'}show()};
el('measure').onclick=async()=>{
 el('measure').disabled=true;el('save').disabled=true;last=null;el('status').textContent='Meten… je hoort drie lage tonen. Blijf even stil.';
 const abort=new AbortController(),timer=setTimeout(()=>abort.abort(),25000);
 try{
  const response=await fetch('/bose/measure',{method:'POST',headers:{'X-Johan2-Probe':'1'},signal:abort.signal,cache:'no-store'});
  const result=await response.json();
  if(!response.ok||!result.valid){el('value').textContent='—';el('status').textContent=result.reason==='busy'?'Johan2 is bezig. Wacht tot het antwoord klaar is en probeer opnieuw.':'Geen betrouwbare meting. Blijf stil en probeer opnieuw; controleer of de Bose aan staat.';return}
  last=result;el('save').disabled=false;el('status').textContent='Meting klaar. Verander de Bose-stand en meet opnieuw om te vergelijken.';show();
 }catch{el('status').textContent='De meting kon niet worden opgehaald. Controleer de verbinding en probeer opnieuw.'}
 finally{clearTimeout(timer);el('measure').disabled=false}
};show();
</script></html>)HTML";
