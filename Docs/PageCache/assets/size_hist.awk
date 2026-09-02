{
  sz=$1;
  if (sz<4096) b="<4K";
  else if (sz<8192) b="4-8K";
  else if (sz<16384) b="8-16K";
  else if (sz<32768) b="16-32K";
  else if (sz<65536) b="32-64K";
  else if (sz<131072) b="64-128K";
  else if (sz<262144) b="128-256K";
  else if (sz<524288) b="256-512K";
  else if (sz<1048576) b="512K-1M";
  else if (sz<4194304) b="1-4M";
  else if (sz<16777216) b="4-16M";
  else if (sz<67108864) b="16-64M";
  else b=">64M";
  cnt[b]++; bytes[b]+=sz; total++; totb+=sz;
}
END {
  for (b in cnt)
    printf "%s\t%d\t%.2f%%\t%.1f MB\t%.2f%%\n", b, cnt[b], 100*cnt[b]/total, bytes[b]/1048576, 100*bytes[b]/totb;
  printf "TOTAL\t%d\t100.00%%\t%.1f MB\t100.00%%\n", total, totb/1048576;
}
