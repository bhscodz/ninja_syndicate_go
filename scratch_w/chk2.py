import sys; sys.path.insert(0,"tools")
from world_catalog import SPR, SHEETS, SHEET_FILE
from PIL import Image, ImageDraw
R="Ninja Adventure - Asset Pack/"
names=[n for n in sys.argv[3:]]
sheets={n:Image.open(R+SHEET_FILE[n]).convert("RGBA") for n in SHEETS}
S=2; cw=8*16*S+8
cols=7
rows=(len(names)+cols-1)//cols
out=Image.new("RGBA",(cols*cw,rows*(8*16*S+16)),(110,150,90,255))
d=ImageDraw.Draw(out)
for i,n in enumerate(names):
    sh,c,r,w,h=SPR[n]
    cr=sheets[sh].crop((c*16,r*16,(c+w)*16,(r+h)*16)).resize((w*16*S,h*16*S),0)
    ox,oy=(i%cols)*cw,(i//cols)*(8*16*S+16)
    out.alpha_composite(cr,(ox+4,oy+14)); d.rectangle([ox+3,oy+13,ox+4+w*16*S,oy+14+h*16*S],outline=(255,0,0,255)); d.text((ox+4,oy+1),n,fill=(0,0,0,255))
out.convert("RGB").save("D:/cloud_computing_game/scratch_w/chk2_%s.png"%sys.argv[1])
