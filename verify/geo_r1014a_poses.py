"""geo_r1014a_poses.py -- free-cam tour poses from a pick string (round 1014 A).

  python verify/geo_r1014a_poses.py name x y z [name x y z ...] > poses.txt

x y z are the numbers a pick prints (`pos x,y,z`). Per pick two poses come out: one
6000 units behind it and 1800 up looking along the route, and one straight above it
(name_top). Feed the result to verify/geo_r1014a_tour.ps1 -PosesFile, with the car
parked beside the geometry (see the tour script).
"""
import json,math,os,sys
ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
d=json.load(open(os.path.join(ROOT,'re','assets','geo',os.environ.get('TD5RE_GEO_PLACE','la_plata'),'_route','ROUTE.JSON')))
P=[(p['x'],p['z']) for p in d['points']]
# usage: tourpose.py name x y z [back up pitch]
def pose(name,x,y,z,back=6000,up=1800,pitch=-12,side=0):
    best=min(range(1,len(P)-1),key=lambda i:(P[i][0]-x)**2+(P[i][1]-z)**2)
    tx=P[best+1][0]-P[best-1][0]; tz=P[best+1][1]-P[best-1][1]
    l=math.hypot(tx,tz); tx/=l; tz/=l
    # lateral +t = (tz,-tx)
    ex=x-tx*back+tz*side; ez=z-tz*back-tx*side
    yaw=math.degrees(math.atan2(tx,tz))
    return '%s:%d,%d,%d,%.1f,%.1f'%(name,ex,y+up,ez,yaw,pitch),best
if __name__=='__main__':
    out=[]
    a=sys.argv[1:]
    i=0
    while i<len(a):
        nm,x,y,z=a[i],float(a[i+1]),float(a[i+2]),float(a[i+3]); i+=4
        s,b=pose(nm,x,y,z); out.append(s); print(nm,'nearest raw node',b,file=sys.stderr)
        s,b=pose(nm+'_top',x,y,z,back=500,up=9000,pitch=-82); out.append(s)
    print(';'.join(out))
