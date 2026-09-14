import time, math, sys
from pymavlink import mavutil
m = mavutil.mavlink_connection("/dev/ttyACM0", baud=115200, source_system=255, source_component=190)
def hb():
    m.mav.heartbeat_send(mavutil.mavlink.MAV_TYPE_GCS, mavutil.mavlink.MAV_AUTOPILOT_INVALID,0,0,0)
t0=time.time(); tgt=None
while time.time()-t0 < 20 and tgt is None:
    hb()
    for _ in range(40):
        try: msg=m.recv_match(blocking=False)
        except Exception: msg=None
        if msg and msg.get_type()=="HEARTBEAT" and msg.get_srcSystem()!=255:
            tgt=(msg.get_srcSystem(), msg.get_srcComponent()); break
        time.sleep(0.02)
if tgt is None: print("no heartbeat from FC"); sys.exit(1)
print("hb from sysid=%d compid=%d" % tgt)
m.mav.command_long_send(tgt[0], tgt[1], mavutil.mavlink.MAV_CMD_SET_MESSAGE_INTERVAL,0,30,50000,0,0,0,0,0)
def a2d(deg,c):
    d=c-int(deg*(10/90.0)); d=max(c-10,min(c+10,d)); return max(5,min(25,d))
t0=time.time(); rows=[]; last_hb=0
while time.time()-t0 < 12:
    if time.time()-last_hb>1: hb(); last_hb=time.time()
    try: msg=m.recv_match(type="ATTITUDE", blocking=False)
    except Exception: msg=None
    if msg is None: time.sleep(0.005); continue
    rows.append((time.time()-t0, math.degrees(msg.roll), math.degrees(msg.pitch)))
if len(rows)<5: print("ATTITUDE samples=%d (too few)"%len(rows)); sys.exit(1)
dur=rows[-1][0]
print("samples=%d dur=%.2fs rate=%.1fHz"%(len(rows),dur,len(rows)/dur))
dts=sorted(rows[i][0]-rows[i-1][0] for i in range(1,len(rows)))
print("dt ms: min=%.1f p50=%.1f p90=%.1f max=%.1f"%(dts[0]*1e3,dts[len(dts)//2]*1e3,dts[int(len(dts)*.9)]*1e3,dts[-1]*1e3))
for name,idx,ctr in (("roll",1,15),("pitch",2,18)):
    v=[r[idx] for r in rows]; mean=sum(v)/len(v)
    sd=(sum((x-mean)**2 for x in v)/len(v))**.5
    du=[a2d(x,ctr) for x in v]
    flips=sum(1 for i in range(1,len(du)) if du[i]!=du[i-1])
    print("%-5s c=%2d mean=%7.2f sd=%5.3f range=[%7.2f,%7.2f] duty=%s flips=%d (%.1f/s)"%(name,ctr,mean,sd,min(v),max(v),sorted(set(du)),flips,flips/dur))
    b=[(x/9.0)%1.0 for x in v]
    print("      frac pos within 9deg step: min=%.3f max=%.3f"%(min(b),max(b)))
