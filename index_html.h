#pragma once
// Web control page, kept in a .h on purpose: the Arduino .ino prototype generator
// scans .ino files and mis-parses the JavaScript "function" definitions inside this
// raw string as C++ (causing "'function' does not name a type"). .h files are not
// scanned that way, so the page lives here and compiles cleanly.
const char INDEX_HTML[] = R"HTML(<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Wall Art</title>
<style>
body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}
h1{font-size:1.2rem}.row{margin:14px 0}
button{font-size:1rem;padding:10px 14px;margin:4px;border:0;border-radius:8px;background:#333;color:#eee}
button.on{background:#2a7}
input[type=range]{width:100%}
input[type=text]{width:100%;font-size:1rem;padding:8px;border-radius:8px;border:0;box-sizing:border-box}
label{display:block;margin:0 0 4px;color:#aaa;font-size:.9rem}
</style></head><body>
<h1>Wall Art Panel</h1>
<div class=row id=power><button onclick="send('power',1);pw(1)">On</button><button onclick="send('power',0);pw(0)">Off</button></div>
<div class=row id=patterns></div>
<div class=row><label>Life cell color</label><select id=lifemode onchange="send('lifemode',this.value)" style="font-size:1rem;padding:8px;border-radius:8px;border:0"></select></div>
<div class=row><label>Message</label><input id=msg type=text>
<button onclick="send('msg',document.getElementById('msg').value)">Set</button></div>
<div class=row><label>Brightness <span id=briv></span></label>
<input id=bri type=range min=0 max=255 oninput="$('briv').textContent=this.value" onchange="send('bri',this.value)"></div>
<div class=row><label>Speed <span id=spdv></span></label>
<input id=spd type=range min=1 max=10 oninput="$('spdv').textContent=this.value" onchange="send('speed',this.value)"></div>
<div class=row><label>Glow <span id=glwv></span></label>
<input id=glw type=range min=1 max=10 oninput="$('glwv').textContent=this.value" onchange="send('glow',this.value)"></div>
<div class=row><label>Freq <span id=frqv></span></label>
<input id=frq type=range min=1 max=10 oninput="$('frqv').textContent=this.value" onchange="send('freq',this.value)"></div>
<div class=row><label>Text color <span id=huev></span></label>
<input id=hue type=range min=0 max=255 oninput="$('huev').textContent=this.value" onchange="send('hue',this.value)"></div>
<div class=row><label><input id=rb type=checkbox onchange="send('rainbow',this.checked?1:0)"> Rainbow text</label></div>
<hr style="border:0;border-top:1px solid #333;margin:18px 0">
<div class=row><label>Panel time: <span id=clock>--:--</span></label></div>
<div class=row><label>Timezone</label><select id=tz onchange="send('tz',this.value)" style="font-size:1rem;padding:8px;border-radius:8px;border:0"></select></div>
<div class=row><label><input id=sched type=checkbox onchange="send('sched',this.checked?1:0)"> Daily on/off schedule</label></div>
<div class=row><label>On at</label><input id=on type=time onchange="send('on',this.value)"></div>
<div class=row><label>Off at</label><input id=off type=time onchange="send('off',this.value)"></div>
<script>
var P=["Twinkle","Rainbow","Text","Analyzer","Starfield","Compute","Clock","Matrix","Life","Plasma","Fire","Tunnel","Scope"];
function $(i){return document.getElementById(i);}
function send(k,v){var p=fetch('/set?'+k+'='+encodeURIComponent(v));if(k=='pattern'){hi(v);p.then(function(){fetch('/state').then(function(r){return r.json();}).then(load);});}}
function hi(p){var b=$('patterns').children;for(var i=0;i<b.length;i++)b[i].className=(b[i].textContent==p)?'on':'';}
function pw(on){var b=$('power').children;b[0].className=on?'on':'';b[1].className=on?'':'on';}
(function(){var h='';for(var i=0;i<P.length;i++)h+='<button onclick="send(\'pattern\',\''+P[i]+'\')">'+P[i]+'</button>';$('patterns').innerHTML=h;})();
function load(s){
 $('msg').value=s.msg;$('bri').value=s.bri;$('briv').textContent=s.bri;
 $('spd').value=s.speed;$('spdv').textContent=s.speed;$('hue').value=s.hue;$('huev').textContent=s.hue;
 $('glw').value=s.glow;$('glwv').textContent=s.glow;$('frq').value=s.freq;$('frqv').textContent=s.freq;
 $('rb').checked=!!s.rainbow;hi(s.pattern);pw(s.power);
 $('clock').textContent=s.time;$('sched').checked=!!s.sched;$('on').value=s.on;$('off').value=s.off;
 var t='';for(var i=0;i<s.tzs.length;i++)t+='<option value="'+i+'"'+(i==s.tz?' selected':'')+'>'+s.tzs[i]+'</option>';$('tz').innerHTML=t;
 var lm='';for(var i=0;i<s.lifemodes.length;i++)lm+='<option value="'+i+'"'+(i==s.lifemode?' selected':'')+'>'+s.lifemodes[i]+'</option>';$('lifemode').innerHTML=lm;
}
fetch('/state').then(function(r){return r.json();}).then(load);
function tick(){fetch('/state').then(function(r){return r.json();}).then(function(s){$('clock').textContent=s.time;});}
setInterval(tick,15000);
</script></body></html>)HTML";
