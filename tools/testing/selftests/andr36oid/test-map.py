from pathlib import Path
import subprocess
k=Path(__file__).resolve().parents[4]
s=(k/'drivers/input/andr36oid-guest.h').read_text();func=s[s.index('static inline unsigned int a36_guest_key'):s.index('static inline void a36_guest_bits')]
code='#include <linux/input.h>\n#include <assert.h>\n#include <stdio.h>\n'+func+'''
int main(void) {
 const unsigned raw[]={304,305,307,308,310,311,312,313,103,108,105,106,314,315,158,125,172};
 const unsigned expected[]={305,304,307,308,310,311,312,313,544,545,546,547,704,705,706,707,708};
 unsigned seen[KEY_MAX+1]={0};
 for(unsigned i=0;i<17;i++){unsigned key=a36_guest_key(raw[i]);assert(key==expected[i]);assert(!seen[key]++);}
 for(unsigned key=704;key<=719;key++)assert(!seen[a36_guest_key(key)]++);
 assert(a36_guest_key(KEY_VOLUMEUP)==KEY_VOLUMEUP);
 assert(a36_guest_key(KEY_POWER)==KEY_POWER);
 puts("PASS: all 17 native buttons, optional extras without collisions, volume/power unchanged");
}
'''
import tempfile
workspace=tempfile.TemporaryDirectory(prefix='a36-native-map-')
p=Path(workspace.name)/'native-map-test.c';p.write_text(code)
subprocess.run(['cc','-Wall','-Wextra','-Werror',str(p),'-o',str(p.with_suffix(''))],check=True)
subprocess.run([str(p.with_suffix(''))],check=True)
