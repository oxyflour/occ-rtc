// End-to-end test against the real OCCT DLL, H.264 encoder and Chromium receiver.
const { chromium } = require('playwright');
const assert = require('node:assert/strict');
const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const output = path.join(root, 'test-results');
fs.mkdirSync(output, { recursive: true });
const port = Number(process.env.DRAWRTC_TEST_PORT || 18080);
const token = 'drawrtc_integration_test_2026';
const bindAddress=process.env.DRAWRTC_TEST_BIND||'127.0.0.1';
const host=process.env.DRAWRTC_TEST_HOST||'127.0.0.1';
const origin=`http://${host}:${port}`;
const powershell = process.env.PWSH || 'powershell.exe';
const report = { tests: [], browserErrors: [] };
let browser, page, pid;
async function test(name, fn) {
  const start = Date.now();
  await fn();
  report.tests.push({ name, passed: true, ms: Date.now() - start });
  console.log('PASS', name);
}
async function tcl(script) {
  return page.evaluate(script => window.drawrtc.command(script), script);
}
async function status() {
  const response = await tcl('rtcstatus');
  assert.equal(response.code, 0, response.result);
  return JSON.parse(response.result);
}
async function waitForVideo() {
  await page.waitForFunction(() => window.drawrtc?.connected && document.querySelector('video').videoWidth > 0, null, { timeout: 30000 });
  await page.waitForFunction(() => (document.querySelector('video').getVideoPlaybackQuality?.().totalVideoFrames || 0) > 2, null, { timeout: 30000 });
}
(async () => {
  // Use a file: DRAW inherits Windows handles and would keep a spawnSync pipe open.
  const launchPath=path.join(output,'launcher.log');
  const launchFd=fs.openSync(launchPath,'w');
  try {
    execFileSync(powershell, ['-NoProfile','-ExecutionPolicy','Bypass','-File',path.join(root,'scripts','run.ps1'),'-BindAddress',bindAddress,'-Port',String(port),'-Token',token], { cwd: root, timeout: 30000, stdio:['ignore',launchFd,launchFd] });
  } finally { fs.closeSync(launchFd); }
  const launch=fs.readFileSync(launchPath,'utf8');
  console.log(launch.trim());
  pid = Number(launch.match(/DRAW PID: (\d+)/)?.[1]);
  assert.ok(pid, 'Launcher must return process id');
  browser = await chromium.launch({ headless: true, args: ['--autoplay-policy=no-user-gesture-required'] });
  page = await browser.newPage({ viewport: { width: 1280, height: 960 }, deviceScaleFactor: 1 });
  page.on('pageerror', error => report.browserErrors.push(error.message));
  page.on('console', message => { if (message.type() === 'error') console.log('BROWSER', message.text()); });
  await test('H.264 WebRTC video and data channel connect', async () => {
    await page.goto(`${origin}/?token=${token}`);
    await waitForVideo();
    const received = await page.evaluate(async () => {
      const results = await window.drawrtc.peer.getStats();
      const inbound = [...results.values()].find(x => x.type === 'inbound-rtp' && x.kind === 'video');
      return { frames: inbound?.framesDecoded, codec: results.get(inbound?.codecId)?.mimeType };
    });
    assert.ok(received.frames > 0);
    assert.equal(received.codec?.toLowerCase(), 'video/h264');
    report.received = received;
  });
  await test('DRAW has no visible native windows', async () => {
    const script = `Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public class DrawRtcWindows {
  public delegate bool Callback(IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(Callback cb, IntPtr l);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr w);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr w, out uint p);
  public static int Visible(uint pid) { int count = 0; EnumWindows((w,l) => { uint p; GetWindowThreadProcessId(w,out p); if(p==pid && IsWindowVisible(w))count++; return true; },IntPtr.Zero);return count; }
}
'@; [DrawRtcWindows]::Visible(${pid})`;
    const count = execFileSync(powershell, ['-NoProfile','-Command',script], { encoding: 'utf8' }).trim();
    assert.equal(count, '0');
  });
  await test('Remote Tcl preserves variables, UTF-8 stdout/stderr and errors', async () => {
    const result = await tcl('set rtc_value 21\nputs "你好 DRAW"\nputs stderr "stderr ok"\nexpr {$rtc_value * 2}');
    assert.equal(result.code, 0, JSON.stringify(result));
    assert.equal(result.result, '42');
    assert.match(result.stdout, /你好 DRAW/);
    assert.match(result.stderr, /stderr ok/);
    const error = await tcl('error "expected test error"');
    assert.equal(error.code, 1);
    assert.match(error.errorInfo, /expected test error/);
    assert.equal((await tcl('set rtc_value')).result, '21');
  });
  await test('Remote DRAW modeling updates the streamed geometry', async () => {
    const result = await tcl('vclear\nbox rtc_box 80 60 40\nvdisplay -dispMode 1 rtc_box\nvsetcolor rtc_box CYAN\nvfit\nset rtc_model_ready 1');
    assert.equal(result.code, 0, result.result);
    assert.equal(result.result, '1');
    await page.waitForFunction(() => {
      const video = document.querySelector('video');
      if (!video.videoWidth) return false;
      const canvas = document.createElement('canvas'); canvas.width = 160; canvas.height = 100;
      const context = canvas.getContext('2d'); context.drawImage(video,0,0,160,100);
      const pixels = context.getImageData(0,0,160,100).data;
      let cyan=0; for(let i=0;i<pixels.length;i+=4)if(pixels[i+1]>pixels[i]+50&&pixels[i+2]>pixels[i]+50)cyan++;
      return cyan>300;
    });
  });
  await test('Pointer selection returns the DRAW object name', async () => {
    const rect = await page.locator('#viewport').boundingBox();
    await page.mouse.click(rect.x+rect.width/2,rect.y+rect.height/2);
    await page.waitForFunction(() => window.drawrtc.selection.includes('rtc_box'));
  });
  await test('Rotate, pan and zoom change the OCCT camera', async () => {
    const rect = await page.locator('#viewport').boundingBox();
    let before = (await status()).camera;
    await page.mouse.move(rect.width*.5,rect.y+rect.height*.5);
    await page.mouse.down(); await page.mouse.move(rect.width*.65,rect.y+rect.height*.6,{steps:10}); await page.mouse.up();
    await page.waitForFunction(previous => JSON.stringify(window.drawrtc.status.camera?.eye)!==JSON.stringify(previous), before.eye);
    let after = (await status()).camera;
    assert.notDeepEqual(after.eye,before.eye);
    before = after;
    await page.mouse.move(rect.width*.5,rect.y+rect.height*.5);
    await page.mouse.down({button:'right'});await page.mouse.move(rect.width*.6,rect.y+rect.height*.55,{steps:8});await page.mouse.up({button:'right'});
    await page.waitForFunction(previous => JSON.stringify(window.drawrtc.status.camera?.center)!==JSON.stringify(previous), before.center);
    after = (await status()).camera;assert.notDeepEqual(after.center,before.center);
    before = after;
    await page.mouse.wheel(0,-180);
    await page.waitForFunction(scale => window.drawrtc.status.camera?.scale!==scale, before.scale);
    after=(await status()).camera;assert.notEqual(after.scale,before.scale);
    await page.click('#fit');
  });
  await test('Browser resizes renegotiate frame dimensions without reconnecting', async () => {
    for (const size of [{width:900,height:720},{width:1451,height:1023},{width:550,height:850}]) {
      await page.setViewportSize(size);
      await page.waitForFunction(() => {
        const element=document.querySelector('#viewport'), video=document.querySelector('video');
        const rect=element.getBoundingClientRect();const ratio=Math.min(1,1920/rect.width,1080/rect.height);
        const w=Math.max(2,Math.floor(rect.width*ratio/2)*2),h=Math.max(2,Math.floor(rect.height*ratio/2)*2);
        return video.videoWidth===w&&video.videoHeight===h;
      },null,{timeout:20000});
      const state=await status();
      assert.equal(state.width%2,0);assert.equal(state.height%2,0);assert.equal(state.lastError,'');
    }
  });
  await test('HTTP status requires the access token', async () => {
    const denied=await fetch(`${origin}/api/status`);assert.equal(denied.status,401);
    const accepted=await fetch(`${origin}/api/status`,{headers:{Authorization:`Bearer ${token}`}});
    assert.equal(accepted.status,200);assert.ok((await accepted.json()).encoded>0);
  });
  await test('Disconnect and reconnect resume both video and Tcl', async () => {
    for(let i=0;i<3;i++) {
      await page.reload();await waitForVideo();assert.equal((await tcl('expr {6*7}')).result,'42');
    }
  });
  await test('Stop and restart release and rebind the ports', async () => {
    await tcl(`after 200 {rtcstop; rtcstart -bind ${bindAddress} -port ${port} -token ${token}}; set rtc_restart scheduled`);
    await page.waitForFunction(() => !window.drawrtc.connected,null,{timeout:15000});
    await waitForVideo();assert.equal((await tcl('set rtc_value')).result,'21');
  });
  await page.setViewportSize({width:1280,height:960});
  await page.waitForFunction(() => document.querySelector('video').videoWidth===1280);
  await tcl('vselect 0 0\nvsetcolor rtc_box STEELBLUE\nvfit');
  await page.waitForFunction(()=>document.querySelector('#metrics').textContent.startsWith('1280'));
  await page.screenshot({path:path.join(output,'drawrtc.png')});
  report.finalStatus=await status();
  assert.equal(report.finalStatus.lastError,'');
  assert.deepEqual(report.browserErrors,[]);
  await tcl('after 100 {rtcstop; set ::drawrtc_shutdown 1}; set rtc_shutdown scheduled');
  report.passed=true;
})().catch(async error => {
  report.passed=false;report.error=error.stack;console.error(error);
  if(page){try{report.client=await page.evaluate(()=>({output:document.querySelector('#output').textContent,state:document.querySelector('#state').textContent,status:window.drawrtc?.status}));await page.screenshot({path:path.join(output,'failure.png')});}catch{}}
  process.exitCode=1;
}).finally(async()=>{
  if(browser)await browser.close();
  if(pid){try{process.kill(pid);}catch{}}
  fs.writeFileSync(path.join(output,'integration.json'),JSON.stringify(report,null,2));
});
