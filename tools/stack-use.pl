open FH, "-|", "gobjdump -d build/zhe.build/Debug/zhe.build/Objects-normal/x86_64/*.o | perl -w ./checkstack.pl x86_64 0" or die "can't run gobjdump|checkstack";
while (<FH>) {
  next unless /^_*([A-Za-z_][A-za-z_0-9]*).*\s+(\d+)\s*$/;
  $sz{$1} = $2;
}
close FH;

open FH, "-|", "cflow --cpp -DNDEBUG -DENABLE_TRACING=0 -Itest -Isrc src/*.c test/*.c 2>/dev/null" or die "can't run cflow";
@max = ([ "", 0 ]);
while (<FH>) {
  next unless /^(\s*)([A-Za-z_][A-Za-z_0-9]*)\(/;
  $level = (length $1) / 4;
  pop @stk while $level < @stk;
  if (exists $sz{$2}) {
    push @stk, [ $2, (@stk ? $stk[-1]->[1] : 0) + $sz{$2} ];
    do { $max = $stk[-1]->[1]; @max = @stk; } if $stk[-1]->[1] > $max[-1]->[1];
  }
}
close FH;

$first = 1;
for (@max) {
  printf "%s %d\n", $_->[0], $_->[1] - ($first ? 0 : $max[0]->[1]);
  print "--\n" if $first;
  $first = 0;
}
