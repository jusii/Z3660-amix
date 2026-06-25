: ${N:=4000}
i=0
while [ $i -lt $N ]; do
  /amixstress 230 4 > /dev/null 2>&1
  /amixstress 150 8 > /dev/null 2>&1
  /amixstress 70 24 > /dev/null 2>&1
  j=0; while [ $j -lt 25 ]; do /bin/true; j=`expr $j + 1`; done
  i=`expr $i + 1`
  if [ `expr $i % 25` -eq 0 ]; then echo "gsoak iter $i `date`" >> /tmp/gsoak.log; fi
done
echo "gsoak DONE iters=$i `date`" >> /tmp/gsoak.log
