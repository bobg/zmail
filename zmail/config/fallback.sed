:join
/\\$/{
    N
    b join
}
/version:/{
    s/^\(.*\)$/"\1",/
    p
}
/^\*/!{/^Zmail\*/!d;}
s/\(\\\n\)[ 	]*/\1/g
s/\("\)/\\\1/g
s/\(\\n\)/\\\1/g
s/^\(.*\)$/"\1",/
