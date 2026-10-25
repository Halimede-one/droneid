"""Optional Playwright smoke test; build all guard profiles first.
All browser requests use simulated USB devices. No serial ports are opened.
Install Playwright/Chromium; optionally set DRONEID_CHROMIUM to a local binary.
"""
import json
import mimetypes
from pathlib import Path
import sys
import os
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'tools'),str(ROOT/'tests')]
import guard_bench as gb
from test_guard_bench import status
from playwright.sync_api import sync_playwright
root=ROOT
catalog=gb.catalog(); bundle=gb.GuardFirmware('xiao_esp32c6').bundle()
s=status(bundle);s['receiver_id']='102030405061';s['fc']['output_frames']=84
radio={'id':'102030405060','serial':'10:20:30:40:50:60','tty':'/dev/ttyACM0','port':'/fake0','device_type':'radio','present':True,'monitoring':True,'connection':'monitoring','status_age_s':0,'contacts':[], 'status':{'configuration_version':1,'broadcast_id':'SCOUT-01','radio_ready':True,'firmware':'0.5.2','role':'both','tx_transport':'wifi_beacon','identify_supported':True}}
guard=dict(radio,id='102030405061',serial='10:20:30:40:50:61',tty='/dev/ttyACM1',device_type='uart_guard',status=s)
guard_c5=dict(guard,id='102030405062',serial='10:20:30:40:50:62',tty='/dev/ttyACM2',status=status(gb.GuardFirmware('xiao_esp32c5').bundle()))
guard_c5['status']['receiver_id']=guard_c5['id']
data={'devices':[radio,guard,guard_c5],'guards':catalog,'guard_fixture':{'available':True,'adapters':[{'id':'a','label':'Sensor adapter · A'},{'id':'b','label':'FC adapter · B'}]},'token':'mock-token','firmware':{'available':True,'version':'0.5.2','size':1100000,'fingerprint':'scout-build'},'server_time':0}
calls=[];errors=[]
radio['usb_input']={'rejected_lines':7,'reader_failures':1,'last_error':'<img src=x onerror="window.badUSB=true">'}
def route(req):
    path=req.request.url.split('8766',1)[1]
    if path=='/api/state':req.fulfill(json=data)
    elif req.request.method=='POST':
        calls.append((path,req.request.post_data_json));req.fulfill(json={'job_id':'mock'})
    elif path.startswith('/api/devices/') and path.endswith('/logs?after=0'):req.fulfill(json={'logs':[],'cursor':0})
    else:
        p=root/'web'/('index.html' if path=='/' else path.lstrip('/'))
        if p.exists():req.fulfill(body=p.read_bytes(),content_type=mimetypes.guess_type(p)[0] or 'text/plain')
        else:req.fulfill(status=404,body='mock: unavailable')
with sync_playwright() as play:
    browser=play.chromium.launch(executable_path=os.environ.get('DRONEID_CHROMIUM'),headless=True,args=['--no-sandbox'])
    page=browser.new_page(viewport={'width':1500,'height':1100})
    page.add_init_script('window.EventSource=class {addEventListener(){}}')
    page.route('**/*',route);page.on('pageerror',lambda e:errors.append(str(e)))
    page.goto('http://127.0.0.1:8766/')
    assert '7 rejected lines' in page.locator('#usb-input-health').inner_text()
    assert page.locator('#usb-input-health img').count()==0
    assert page.evaluate('window.badUSB === undefined')
    assert page.locator('[role="tab"]').count()==3
    assert page.locator('#tab-guards').count()==0
    page.locator('#tab-settings').click()
    assert page.locator('#device-role-scout').is_checked()
    assert page.locator('#scout-settings').is_visible()
    page.check('#device-role-guard')
    assert not calls, 'Role selection must not write to the device'
    assert page.locator('#guard-settings').is_visible() and page.locator('#scout-settings').is_hidden()
    assert page.locator('#running-role').inner_text()=='Running: Scout'
    assert page.locator('#role-change-note').is_visible()
    assert page.locator('#guard-flash').is_disabled()
    page.select_option('#guard-model','xiao_esp32c5')
    page.locator('#guard-flash').click()
    assert calls[-1][0].endswith('/102030405060/provision') and calls[-1][1]['role']=='guard'
    page.locator('[data-device="102030405061"]').click()
    assert page.locator('#panel-settings').is_visible()
    assert page.locator('#device-role-guard').is_checked()
    assert page.locator('#guard-model').input_value()=='xiao_esp32c6'
    count=len(calls)
    page.check('#device-role-scout')
    assert page.locator('#running-role').inner_text()=='Running: Guard'
    assert page.locator('#flash').is_disabled(), 'C6 cannot run Scout firmware'
    assert len(calls)==count
    page.check('#device-role-guard')
    page.locator('#guard-check').click()
    assert calls[-1][0].endswith('/102030405061/guard-check')
    page.locator('.guard-fixture summary').click()
    page.select_option('#guard-sensor-adapter','a');page.select_option('#guard-fc-adapter','a')
    page.check('#guard-fixture-confirm');assert page.locator('#guard-uart-test').is_disabled()
    page.select_option('#guard-fc-adapter','b');assert page.locator('#guard-uart-test').is_enabled()
    page.locator('#guard-uart-test').click();assert calls[-1][1]['sensor_adapter']=='a'
    page.locator('.guard-fixture summary').click()
    page.evaluate('window.scrollTo(0,0)')
    page.screenshot(path='/tmp/guard-desktop.png',full_page=True)
    page.locator('[data-device="102030405062"]').click()
    assert not page.locator('#guard-fixture-confirm').is_checked()
    page.check('#device-role-scout')
    assert page.locator('#role-change-note').is_visible()
    assert page.locator('#broadcast-id').is_disabled(), 'Install Scout before configuring it'
    page.locator('#flash').click()
    assert calls[-1][0].endswith('/102030405062/provision') and calls[-1][1]['role']=='scout'
    # Device-local drafts survive selection changes; no implicit role mutation.
    page.locator('[data-device="102030405060"]').click()
    assert page.locator('#device-role-guard').is_checked()
    page.check('#device-role-scout')
    assert page.locator('#running-role').inner_text()=='Running: Scout'
    page.evaluate('window.scrollTo(0,0)')
    page.screenshot(path='/tmp/scout-settings-desktop.png',full_page=True)
    page.locator('#tab-settings').focus();page.keyboard.press('ArrowRight')
    assert page.locator('#tab-nearby').get_attribute('aria-selected')=='true'
    page.locator('[data-device="102030405061"]').click()
    page.set_viewport_size({'width':390,'height':844})
    page.evaluate('window.scrollTo(0,0)')
    page.screenshot(path='/tmp/guard-mobile.png',full_page=True)
    assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth'), 'Horizontal overflow'
    page.check('#device-role-scout')
    assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth'), 'Scout horizontal overflow'
    assert not errors,errors
    print(json.dumps({'browser_errors':errors,'routes_verified':calls,'responsive_width':390}))
    browser.close()
