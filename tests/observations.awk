{ if (NF != 10 || $5 != 1 || $6 != 1 || $7 != 1 || $8 > 4096 || $10-$9 > 8192) bad=1 }
NR==1 || NR==6 { if ($1 != high || $2 != low || $3 != high || $4 != low) bad=1 }
NR==2 || NR==5 { if ($1 || $2 || $3 || $4) bad=1 }
NR==3 { if ($1 || $2 != 64 || $3 || $4) bad=1 }
NR==4 { if ($1 != high || $2 != low || $3 || $4 != 4096) bad=1 }
END { if (NR != rows || bad) { print "Invalid bounded-stream observations" > "/dev/stderr"; exit 1 } }
