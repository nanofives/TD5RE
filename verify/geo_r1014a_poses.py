import json,math,sys
d=json.load(open('C:/Users/maria/Desktop/Proyectos/TD5RE/.claude/worktrees/fix-1791574676-4155-29020/re/assets/geo/la_plata/_route/ROUTE.JSON'))
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
