"""Verify two-axis scrollbar input and pixels while the renderer is occupied."""
import json
import re
import time

def run(execute,build,healthy,move,click,key,wait_marker,type_address):
    key('ctrl','l');type_address('http://10.0.2.2:8765/scroll-controls.html');key('ret')
    wait_marker('CONTROLS ready');time.sleep(2)
    def capture(name):
        destination=build/('input-controls-'+name+'.ppm')
        execute('screendump',{'filename':str(destination)})
        parts=destination.read_bytes().split(b'\n',3)
        return int(parts[1].split()[0]),parts[3]
    def crop(image,left,top,right,bottom):
        width,pixels=image
        return b''.join(pixels[(row*width+left)*3:(row*width+right)*3] for row in range(top,bottom))
    def press(down):
        execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
        time.sleep(.08)
    def positions(text):
        return [(kind,int(y),int(element),int(x)) for kind,y,element,x in re.findall(
            r'COMPOSITOR: async (\w+) sequence=\d+ position=(-?\d+) element=(\d+) x=(-?\d+)',text)]
    def busy(generation=1):
        assert 'CONTROLS end '+str(generation) not in healthy(),'Renderer finished before verification'
    measurements=[]
    known_positions={}
    def action(name,operation,axis,generation=1,region=(60,250,560,650)):
        start=len(healthy());before=capture(name+'-before');began=time.monotonic();operation()
        deadline=time.monotonic()+1
        while time.monotonic()<deadline:
            observed=positions(healthy()[start:])
            if observed:break
            time.sleep(.01)
        else:raise AssertionError(name+' produced no asynchronous command: '+healthy()[start:])
        after=capture(name+'-busy');busy(generation)
        left=crop(before,*region);right=crop(after,*region)
        assert sum(abs(a-b)>2 for a,b in zip(left,right))>100,name+' did not change scroll pixels'
        last=observed[-1];previous=known_positions.get(last[2],(0,0))
        assert last[1 if axis=='x' else 3]==previous[1 if axis=='x' else 0],name+' changed the other axis'
        known_positions[last[2]]=(last[3],last[1])
        measurements.append({'action':name,'axis':axis,'milliseconds':round((time.monotonic()-began)*1000,2),'last':observed[-1]})
        return observed
    capture('initial')
    click(130,45);wait_marker('CONTROLS begin 1')
    action('key-right',lambda:execute('send-key',{'keys':[{'type':'qcode','data':'right'}]}),'x')
    first_key_ms=measurements[-1]['milliseconds'];time.sleep(.2)
    action('key-left',lambda:key('left'),'x')
    action('horizontal-forward-start',lambda:click(96,558),'x')
    action('horizontal-back-start',lambda:click(72,558),'x')
    action('horizontal-forward-end',lambda:click(524,558),'x')
    action('horizontal-back-end',lambda:click(500,558),'x')
    action('vertical-forward-start',lambda:click(548,206),'y')
    action('vertical-back-start',lambda:click(548,182),'y')
    action('vertical-forward-end',lambda:click(548,534),'y')
    action('vertical-back-end',lambda:click(548,510),'y')
    action('horizontal-track-forward',lambda:click(430,558),'x')
    action('horizontal-track-back',lambda:click(115,558),'x')
    action('vertical-track-forward',lambda:click(548,470),'y')
    action('vertical-track-back',lambda:click(548,225),'y')
    def drag_horizontal():
        move(145,558);press(True);move(350,558);press(False)
    action('horizontal-thumb',drag_horizontal,'x')
    def drag_vertical():
        move(548,235);press(True);move(548,400);press(False)
    action('vertical-thumb',drag_vertical,'y')
    move(524,558);start=len(healthy());press(True);time.sleep(.75)
    repeated=positions(healthy()[start:]);assert sum(row[0]=='repeat' for row in repeated)>=2,repeated
    move(600,600);pause=len(healthy());time.sleep(.35)
    assert not positions(healthy()[pause:]),'Autorepeat did not pause outside the scrollbar'
    press(False);released=len(healthy());time.sleep(.25)
    assert not positions(healthy()[released:]),'Autorepeat continued after release'
    busy();busy_frame=capture('final-busy')
    custom_final=positions(healthy()[start:])[-1];wait_marker('CONTROLS end 1')
    wait_marker('CONTROLS offsets custom='+str(custom_final[3])+','+str(custom_final[1]));time.sleep(2)
    reconciled=capture('reconciled')
    before=crop(busy_frame,60,250,560,650);after=crop(reconciled,60,250,560,650)
    mismatch=sum(abs(a-b)>2 for a,b in zip(before,after))/len(before)
    assert mismatch<.02,('Reconciliation changed scroll geometry',mismatch)
    known_positions.clear()
    click(130,45);wait_marker('CONTROLS begin 2')
    native_region=(700,250,1200,650)
    action('native-horizontal-arrow',lambda:click(1177,562),'x',generation=2,region=native_region)
    action('native-horizontal-track',lambda:click(1100,562),'x',generation=2,region=native_region)
    action('native-vertical-arrow',lambda:click(1192,546),'y',generation=2,region=native_region)
    action('native-vertical-track',lambda:click(1192,480),'y',generation=2,region=native_region)
    # A held track click stops when the thumb reaches the pointer.
    move(1050,562);start=len(healthy());press(True);time.sleep(1.2)
    track_commands=positions(healthy()[start:]);assert track_commands,healthy()[start:]
    stopped=len(healthy());time.sleep(.3)
    assert not positions(healthy()[stopped:]),'Track repeat continued after reaching the pointer'
    press(False);busy(2)
    def root_horizontal():
        move(140,712);press(True);move(400,712);press(False)
    action('root-horizontal-thumb',root_horizontal,'x',generation=2,region=(0,780,1280,800))
    native_final=track_commands[-1];root_final=measurements[-1]['last']
    native_busy=capture('native-busy');wait_marker('CONTROLS end 2')
    wait_marker('native='+str(native_final[3])+','+str(native_final[1])+' root='+str(root_final[3])+','+str(root_final[1]))
    time.sleep(2);native_reconciled=capture('native-reconciled')
    before=crop(native_busy,*native_region);after=crop(native_reconciled,*native_region)
    native_mismatch=sum(abs(a-b)>2 for a,b in zip(before,after))/len(before)
    assert native_mismatch<.02,('Native reconciliation changed geometry',native_mismatch)
    known_positions.clear()
    click(510,45);wait_marker('CONTROLS begin 3')
    action('rtl-key-left',lambda:key('left'),'x',generation=3)
    assert measurements[-1]['last'][3]==-40,measurements[-1]
    action('rtl-key-right',lambda:key('right'),'x',generation=3)
    assert measurements[-1]['last'][3]==0,measurements[-1]
    def rtl_drag():
        move(470,558);press(True);move(260,558);press(False)
    action('rtl-horizontal-thumb',rtl_drag,'x',generation=3)
    rtl_final=measurements[-1]['last'][3];assert rtl_final<0,rtl_final
    rtl_busy=capture('rtl-busy');wait_marker('CONTROLS end 3')
    wait_marker('CONTROLS offsets custom='+str(rtl_final)+',0');time.sleep(2)
    rtl_reconciled=capture('rtl-reconciled')
    before=crop(rtl_busy,60,250,560,650);after=crop(rtl_reconciled,60,250,560,650)
    rtl_mismatch=sum(abs(a-b)>2 for a,b in zip(before,after))/len(before)
    assert rtl_mismatch<.02,('RTL reconciliation changed geometry',rtl_mismatch)
    result={'first_horizontal_key_capture_ms':first_key_ms,'javascript_block_ms':20000,'actions':measurements,'repeat_commands':sum(row[0]=='repeat' for row in repeated),
            'repeat_pause_and_release':True,'reconciliation_difference_fraction':mismatch,'final_custom_x':custom_final[3],'final_custom_y':custom_final[1],'track_repeat_stops_at_pointer':True,'native_reconciliation_difference_fraction':native_mismatch,'rtl_final_x':rtl_final,'rtl_reconciliation_difference_fraction':rtl_mismatch}
    (build/'scroll-controls-results.json').write_text(json.dumps(result,indent=2))
    print('PASS: custom scrollbar arrows, tracks, dragging and horizontal keys during JavaScript',json.dumps(result))
