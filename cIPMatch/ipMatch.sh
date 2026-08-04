#!/bin/bash
#
# ipMatch.sh -- find the full cluster of accounts and IP addresses connected
#               to a given account, through shared IP addresses.
#
# Usage:  ./ipMatch.sh ipLog.txt someone@example.com
#
# The ipLog is expected to have one "IP_ADDRESS ACCOUNT_EMAIL" pair per line.
#
# Starting from the given account, we collect every IP that account used,
# then every account seen on any of those IPs, then every IP used by any of
# those accounts, and so on, until neither list grows any more.  The result
# is the connected component containing the starting account.

if [ $# -ne 2 ]; then
    echo "Usage: $0 ipLogFile accountEmail" >&2
    exit 1
fi

logFile="$1"
seedAccount="$2"

if [ ! -r "$logFile" ]; then
    echo "$0: cannot read ip log file '$logFile'" >&2
    exit 1
fi


result=$( awk -v seed="$seedAccount" '
{
    ip = $1
    account = $2

    # Skip duplicate (ip, account) pairs so the adjacency lists stay small.
    pairKey = ip SUBSEP account
    if ( pairKey in seenPair ) next
    seenPair[ pairKey ] = 1

    # accountsOnIP[ ip, n ] and ipsOfAccount[ account, n ] act as the
    # two halves of the bipartite graph we are about to walk.
    accountsOnIP[ ip, ++ipCount[ ip ] ] = account
    ipsOfAccount[ account, ++accountCount[ account ] ] = ip
}

END {
    if ( ! ( seed in accountCount ) ) {
        print "warning: account " seed " does not appear in the ip log" \
            > "/dev/stderr"
    }

    # Breadth first walk over the two queues.  An entry is only ever
    # queued once, the moment it is first discovered, so each account and
    # each IP is expanded exactly one time.
    foundAccount[ seed ] = 1
    accountQueue[ ++accountQueueEnd ] = seed

    while ( accountQueueNext < accountQueueEnd || ipQueueNext < ipQueueEnd ) {

        # Every IP used by a newly found account is itself newly found.
        while ( accountQueueNext < accountQueueEnd ) {
            account = accountQueue[ ++accountQueueNext ]

            for ( i = 1; i <= accountCount[ account ]; i++ ) {
                ip = ipsOfAccount[ account, i ]

                if ( ! ( ip in foundIP ) ) {
                    foundIP[ ip ] = 1
                    ipQueue[ ++ipQueueEnd ] = ip
                }
            }
        }

        # Every account seen on a newly found IP is itself newly found.
        while ( ipQueueNext < ipQueueEnd ) {
            ip = ipQueue[ ++ipQueueNext ]

            for ( i = 1; i <= ipCount[ ip ]; i++ ) {
                account = accountsOnIP[ ip, i ]

                if ( ! ( account in foundAccount ) ) {
                    foundAccount[ account ] = 1
                    accountQueue[ ++accountQueueEnd ] = account
                }
            }
        }
    }

    # Tag the two kinds of output so the shell can split them apart and
    # sort each list on its own.
    for ( account in foundAccount ) print "A", account
    for ( ip in foundIP ) print "I", ip
}
' "$logFile" )

if [ $? -ne 0 ]; then
    exit 1
fi


accounts=$( echo "$result" | awk '$1 == "A" { print $2 }' | sort )
# Sort IPs numerically by octet, so 9.x lands before 10.x.
ips=$( echo "$result" | awk '$1 == "I" { print $2 }' |
       sort -t . -k1,1n -k2,2n -k3,3n -k4,4n )

accountTotal=$( echo "$accounts" | grep -c . )
ipTotal=$( echo "$ips" | grep -c . )


echo "Connected accounts ($accountTotal):"
echo "$accounts"
echo ""
echo "Connected IP addresses ($ipTotal):"
echo "$ips"
