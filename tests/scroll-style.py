"""Check scrollbar state pixels through firmware input during a long JavaScript task."""
import json
import time

def run(execute,build,healthy,move,click,key,wait_marker,type_address):
    key('ctrl','l');type_address('http://10.0.2.2:8765/scroll-style.html');key('ret')
    wait_marker('STYLE ready');time.sleep(2)
    def capture(name):
        destination=build/('input-style-'+name+'.ppm');execute('screendump',{'filename':str(destination)})
        parts=destination.read_bytes().split(b'\n',3)
        return int(parts[1].split()[0]),parts[3]
    def crop(image,rectangle):
        width,pixels=image;left,top,right,bottom=rectangle
        return b''.join(pixels[(y*width+left)*3:(y*width+right)*3] for y in range(top,bottom))
    checks=[]
    def color(name,rectangle,rgb):
        image=capture(name);pixels=crop(image,rectangle)
        count=sum(pixels[index:index+3]==bytes(rgb) for index in range(0,len(pixels),3))
        assert count>30,(name,count,rgb)
        assert 'STYLE end' not in healthy(),name+' happened after JavaScript completed'
        checks.append({'state':name,'matching_pixels':count});return image
    def press(down):
        execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.12)
    vertical=(536,230,560,586);horizontal=(60,586,536,610)
    initial=capture('initial');click(130,42);wait_marker('STYLE capabilities hover=true fine=true');wait_marker('STYLE begin')
    move(548,213);color('thumb-hover',vertical,(204,51,204))
    press(True);color('thumb-pressed-hover',vertical,(34,204,204))
    move(600,213);color('thumb-pressed-outside',vertical,(34,187,102))
    press(False);color('thumb-released',vertical,(238,85,34))
    move(145,518);color('horizontal-thumb-hover',horizontal,(204,51,204))
    press(True);color('horizontal-thumb-pressed',horizontal,(34,204,204))
    press(False);color('horizontal-thumb-released-hover',horizontal,(204,51,204))
    move(548,162);color('button-hover',(536,230,560,254),(238,187,34))
    press(True);color('button-pressed',(536,230,560,254),(136,68,204))
    press(False);color('button-released-hover',(536,230,560,254),(238,187,34))
    move(548,350);color('track-hover',vertical,(221,153,85))
    press(True);color('track-pressed',vertical,(119,85,221));press(False)
    move(600,400);color('track-leave',vertical,(153,221,187))
    move(72,678);color('disabled-pointer',(60,746,108,770),(119,119,119))
    start=len(healthy());press(True);color('disabled-pressed',(60,746,108,770),(119,119,119));press(False)
    assert 'COMPOSITOR: async' not in healthy()[start:],'Disabled scrollbar changed an offset'
    move(600,400);color('disabled-leave',(60,746,108,770),(119,119,119))
    native_rectangle=(1186,248,1191,268)
    before=capture('native-normal');move(1192,180);hover=capture('native-hover')
    assert crop(before,native_rectangle)!=crop(hover,native_rectangle),'Native hover did not change'
    press(True);pressed=capture('native-pressed')
    assert crop(pressed,native_rectangle)!=crop(hover,native_rectangle),'Native pressed state did not change'
    move(1192,560);at_end=capture('native-end-pressed');press(False);move(600,400)
    bottom=capture('native-end')
    arrow=(1184,578,1200,594)
    assert crop(initial,arrow)!=crop(bottom,arrow),'Native end arrow did not become disabled'
    # Pointer motion into the toolbar must clear page hover without waiting for Blink.
    move(548,162);color('before-toolbar-leave',(536,230,560,254),(238,187,34))
    move(500,35,page=False);color('toolbar-leave',(536,230,560,254),(68,136,204))
    assert 'STYLE end' not in healthy()
    final_busy=capture('final-busy');wait_marker('STYLE end');time.sleep(2);reconciled=capture('reconciled')
    region=(60,230,1200,770);left=crop(final_busy,region);right=crop(reconciled,region)
    mismatch=sum(abs(a-b)>2 for a,b in zip(left,right))/len(left)
    assert mismatch<.02,('State reconciliation differs',mismatch)
    result={'javascript_block_ms':25000,'checks':checks,'native_hover_and_pressed':True,'native_boundary_disabled':True,
            'disabled_input_ignored':True,'toolbar_leave_clears_hover':True,'reconciliation_difference_fraction':mismatch}
    (build/'scroll-style-results.json').write_text(json.dumps(result,indent=2)+'\n')
    print('PASS: asynchronous scrollbar appearance',json.dumps(result))
