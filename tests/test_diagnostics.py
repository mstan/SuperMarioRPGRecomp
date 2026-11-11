"""Private-ROM, real desktop-host recording/replay and crash-survival checks."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
import zipfile

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('bundle','rom','out'):p.add_argument('--'+name,type=Path,required=True)
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=False)
    rom=a.rom.resolve();assert hashlib.sha256(rom.read_bytes()).hexdigest()=='740646f3535bfb365ca44e70d46ab433467b142bd84010393070bd0b141af853'
    template=out/'template';template.mkdir()
    with zipfile.ZipFile(a.bundle.resolve()) as z:
        assert z.testzip() is None;z.extractall(template)
    clean={k:v for k,v in os.environ.items() if not k.startswith(('SMRPG_','SNESRECOMP_','SDL_','LNG_'))}
    clean.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy',SMRPG_TEST_HIDDEN='1',SNESRECOMP_SAVE_ROOT='saves')
    flags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0
    def install(name):
        folder=out/name;shutil.copytree(template,folder);return folder
    def run(folder,label,extra=None,pulse=False,args=None):
        env=dict(clean,SNESRECOMP_AUTOCLOSE_FRAMES='180');env.update(extra or {})
        with socket.socket() as probe:probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
        env['SNESRECOMP_DEBUG_PORT']=str(port)
        if pulse:env['SNESRECOMP_INPUT_SCRIPT']='10-20:0x10'
        command=[str(folder/'SuperMarioRPGSNESRecomp.exe'),str(rom),'--no-launcher'] if args is None else args
        with (folder/(label+'.log')).open('wb') as log:
            child=subprocess.Popen(command,cwd=folder,env=env,stdout=log,stderr=log,creationflags=flags)
            try:code=child.wait(timeout=90)
            except BaseException:child.terminate();child.wait(timeout=10);raise
        return code,(folder/(label+'.log')).read_text(errors='replace')
    optional=install('optional')
    code,text=run(optional,'default',{'SNESRECOMP_AUTOCLOSE_FRAMES':'30'})
    assert code==0 and not list((optional/'logs').glob('*.replay'))
    state=optional/'mods/preloaded/state.toml'
    setting='package_id = "super-mario-rpg.diagnostics.replay"\nid = "replay-capture"\nenabled = false'
    settings=state.read_text();assert setting in settings
    state.write_text(settings.replace(setting,setting.replace('false','true')))
    code,text=run(optional,'mod-enabled',{'SNESRECOMP_AUTOCLOSE_FRAMES':'30'})
    assert code==0 and '[smrpg-replay] recording ' in text
    assert len(list((optional/'logs').glob('*.replay')))==1
    print('PASS: fresh capture is off; the actual Mods selection enables recording',flush=True)
    normal=install('normal');(normal/'saves').mkdir();marker=normal/'saves/keep.srm';marker.write_bytes(b'untouched native save sentinel')
    code,text=run(normal,'record',{'SMRPG_REPLAY_RECORD':'1','SMRPG_REPLAY_DIR':str(normal/'logs')},pulse=True)
    assert code==0 and '[smrpg] Renderer: custom widescreen' in text,(code,text[-3000:])
    recording=next((normal/'logs').glob('*.replay'));lines=recording.read_text().splitlines()
    assert sum(s.startswith('I ') for s in lines)==sum(s.startswith('H ') for s in lines)==180
    assert any(int(s.split()[2],16)&0xfff for s in lines if s.startswith('I ')), 'No actual controller input was recorded'
    code,text=run(normal,'playback',{'SMRPG_REPLAY_RECORD':'0','SMRPG_REPLAY_INPUT_FILE':str(recording),'SNESRECOMP_AUTOCLOSE_FRAMES':'0'})
    assert code==0 and 'all recorded checkpoints matched (180 frames)' in text,(code,text[-3000:])
    assert marker.read_bytes()==b'untouched native save sentinel'
    print('PASS: real inputs, SA-1 recording, 180-frame replay, fresh Fit default and native save preservation',flush=True)
    early=install('early-crash')
    code,text=run(early,'startup-crash',{'SMRPG_CRASH_TEST_PHASE':'startup'},args=[str(early/'SuperMarioRPGSNESRecomp.exe'),'--help'])
    assert code!=0 and 'intentional diagnostic crash drill: startup' in text
    copies=list(early.glob('crash_report_*.json'));assert copies and '0.0.6' in copies[0].read_text()
    preserved={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in copies}
    code,_=run(early,'later-help',args=[str(early/'SuperMarioRPGSNESRecomp.exe'),'--help']);assert code==0
    assert preserved=={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in copies}
    print('PASS: pre-ROM/pre-SDL crash report and preservation after another launch',flush=True)
    crash=install('frame-crash')
    code,text=run(crash,'crash',{'SMRPG_REPLAY_RECORD':'1','SMRPG_REPLAY_DIR':str(crash/'logs'),'SNESRECOMP_CRASH_TEST':'seh','SNESRECOMP_AUTOCLOSE_FRAMES':'300'})
    assert code!=0 and list(crash.glob('crash_report_*.json'))
    raw=next((crash/'logs').glob('*.replay'));chunks=raw.read_bytes().splitlines(keepends=True)
    completed=[i for i,s in enumerate(chunks) if s.startswith(b'H ')];assert len(completed)>=60,len(completed)
    prefix=raw.with_name('completed-prefix.replay');prefix.write_bytes(b''.join(chunks[:completed[-1]+1]))
    for suffix in ('.sav','.mods.txt','.mods.toml','.config.ini'):
        sidecar=Path(str(raw)+suffix)
        if sidecar.exists():shutil.copy2(sidecar,Path(str(prefix)+suffix))
    code,text=run(crash,'prefix-playback',{'SMRPG_REPLAY_RECORD':'0','SMRPG_REPLAY_INPUT_FILE':str(prefix),'SNESRECOMP_AUTOCLOSE_FRAMES':'0'})
    assert code==0 and f'all recorded checkpoints matched ({len(completed)} frames)' in text,(code,text[-3000:])
    print('PASS: crash-surviving completed prefix replays with all checkpoints matching',flush=True)
    if os.name=='nt':
        with (normal/'wrapper-test.log').open('wb') as log:
            subprocess.run([os.environ.get('COMSPEC','C:/Windows/System32/cmd.exe'),'/d','/c','call','Start diagnostics.cmd','--help'],cwd=normal,env=dict(clean),input=b'\n',stdout=log,stderr=log,check=True,timeout=30,creationflags=flags)
        launch=next((normal/'logs').glob('startup-*.log')).read_text(errors='replace')
        assert 'diagnostic launch' in launch and 'Exit code: 0' in launch
        subprocess.run(['C:/Windows/System32/WindowsPowerShell/v1.0/powershell.exe','-NoProfile','-File',str(normal/'Collect diagnostics.ps1')],cwd=normal,env=clean,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,check=True,timeout=90,creationflags=flags)
        collection=next(normal.glob('smrpg-diagnostics-*.zip'))
        with zipfile.ZipFile(collection) as z:assert z.testzip() is None and not any(s.lower().endswith(('.sfc','.smc')) for s in z.namelist())
        assert marker.read_bytes()==b'untouched native save sentinel'
        print('PASS: outer startup launcher and collector; originals preserved and no ROM file included',flush=True)
    (out/'validation.json').write_text(json.dumps(dict(recorded_frames=180,matching_replay_frames=180,crash_prefix_frames=len(completed),startup_crash=True,preserved_crash_copy=True,fresh_widescreen_default=True,native_saves_preserved=True,host=os.environ.get('COMPUTERNAME','unknown')),indent=2)+'\n')

if __name__=='__main__':main()
