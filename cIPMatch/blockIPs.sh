#!/bin/bash
#
# blockIPs.sh -- add DROP rules to a Linode (Akamai) cloud firewall, covering
#                all TCP ports, for a list of IP addresses.
#
# Usage:  ./blockIPs.sh FIREWALL_ID AUTH_TOKEN IP_LIST_FILE [--dry-run]
#
#   FIREWALL_ID    numeric id, as seen in the cloud manager URL
#                  https://cloud.linode.com/firewalls/105276419  ->  105276419
#   AUTH_TOKEN     a Linode personal access token with read/write on firewalls.
#                  May instead be the path to a file containing the token,
#                  which keeps the token out of your shell history and out of
#                  the process list that any other user on the box can read.
#   IP_LIST_FILE   one address per line.  Bare addresses get an implicit /32
#                  (or /128 for IPv6); explicit CIDR ranges are also accepted.
#                  Blank lines, and lines starting with #, are ignored.
#
#   --dry-run      show exactly what would change, send nothing.
#
# The Linode rules endpoint replaces the entire rule set on every write, so
# this script reads the current rules first and merges into them.  Rules it
# did not create are passed through untouched, in their original order.
#
# The rules this script owns are labelled with a common prefix (see
# labelPrefix below).  On each run it gathers the addresses already in those
# rules, adds the new ones, and rebuilds the whole owned set repacked into
# full chunks.  So re-running with a longer list never leaves half empty
# rules behind, and re-running with the same list changes nothing at all.
#
# The owned DROP rules are placed at the front of the inbound list, because
# Linode evaluates rules in order and the first match wins.  Appending them
# instead would let any pre-existing ACCEPT rule admit a blocked address
# before the DROP was ever reached.

labelPrefix="blockedIP"

# Firewall limits, confirmed against the live API.  Both are enforced by
# Linode, not by us -- we check them early only to fail with a message that
# says what to do about it.
maxAddressesPerRule=255
maxRulesPerFirewall=25


scriptName=$( basename "$0" )

if [ $# -lt 3 ] || [ $# -gt 4 ]; then
    echo "Usage: $scriptName FIREWALL_ID AUTH_TOKEN IP_LIST_FILE [--dry-run]" >&2
    exit 1
fi

firewallID="$1"
authToken="$2"
ipListFile="$3"
dryRun="$4"

if [ -n "$dryRun" ] && [ "$dryRun" != "--dry-run" ]; then
    echo "$scriptName: unknown option '$dryRun'" >&2
    exit 1
fi

case "$firewallID" in
    ''|*[!0-9]*)
        echo "$scriptName: firewall id '$firewallID' is not a number" >&2
        exit 1 ;;
esac

if [ ! -r "$ipListFile" ]; then
    echo "$scriptName: cannot read ip list file '$ipListFile'" >&2
    exit 1
fi

for needed in curl python3; do
    if ! command -v "$needed" > /dev/null 2>&1; then
        echo "$scriptName: $needed is required but not installed" >&2
        exit 1
    fi
done

# A token argument that names a readable file means the token is inside it.
if [ -f "$authToken" ]; then
    authToken=$( head -n 1 "$authToken" | tr -d ' \t\r\n' )

    if [ -z "$authToken" ]; then
        echo "$scriptName: token file '$2' is empty" >&2
        exit 1
    fi
fi


apiURL="https://api.linode.com/v4/networking/firewalls/$firewallID/rules"

# Keep the token in a header file rather than on the curl command line, where
# it would be visible in ps output for as long as the request runs.
workDir=$( mktemp -d ) || exit 1
trap 'rm -rf "$workDir"' EXIT

headerFile="$workDir/headers"
( umask 077; printf 'Authorization: Bearer %s\n' "$authToken" > "$headerFile" )


# Reads the current rules.  Any HTTP status other than 200 is fatal, and we
# show Linode's own error text, which is far more specific than anything we
# could infer from the status alone.
httpStatus=$( curl -s -o "$workDir/current.json" -w '%{http_code}' \
                   -H @"$headerFile" "$apiURL" )

if [ "$httpStatus" != "200" ]; then
    echo "$scriptName: could not read firewall $firewallID (HTTP $httpStatus)" >&2

    case "$httpStatus" in
        401) echo "  the auth token was rejected" >&2 ;;
        404) echo "  no such firewall, or the token cannot see it" >&2 ;;
    esac

    head -c 500 "$workDir/current.json" >&2
    echo >&2
    exit 1
fi


# Builds the merged rule set.  Prints the summary on stderr for the user and
# writes the request body to newRules.json.  Exits non zero, having explained
# itself, if the merge cannot be done.
python3 - "$workDir/current.json" "$ipListFile" "$workDir/newRules.json" \
         "$labelPrefix" "$maxAddressesPerRule" "$maxRulesPerFirewall" <<'PYTHON_END'
import json, re, sys

currentFile, ipListFile, outputFile = sys.argv[1], sys.argv[2], sys.argv[3]
labelPrefix = sys.argv[4]
maxAddresses = int( sys.argv[5] )
maxRules = int( sys.argv[6] )

def fail( message ):
    sys.stderr.write( "error: " + message + "\n" )
    sys.exit( 1 )

try:
    current = json.load( open( currentFile ) )
except ValueError:
    fail( "the firewall returned something that is not JSON" )


ipv4Pattern = re.compile( r"^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})"
                          r"(?:/(\d{1,2}))?$" )

def normalize( address, lineNumber ):
    """Returns ( family, cidr ).  Bare addresses gain a full width mask."""

    if ":" in address:
        # IPv6.  Left to the API to validate in detail; we only sort it into
        # the right bucket and supply a mask if one is missing.
        if "/" not in address:
            address += "/128"
        return ( "ipv6", address )

    match = ipv4Pattern.match( address )

    if match is None:
        fail( "line %d: '%s' is not an IP address" % ( lineNumber, address ) )

    for octet in match.group( 1, 2, 3, 4 ):
        if int( octet ) > 255:
            fail( "line %d: '%s' has an octet above 255"
                  % ( lineNumber, address ) )

    mask = match.group( 5 )

    if mask is None:
        return ( "ipv4", address + "/32" )

    if int( mask ) > 32:
        fail( "line %d: '%s' has a prefix length above 32"
              % ( lineNumber, address ) )

    return ( "ipv4", address )


# Reads the requested addresses, dropping duplicates but keeping file order,
# so the resulting rules read in the same order as the list the user supplied.
requested = { "ipv4": [], "ipv6": [] }
seen = set()

for lineNumber, line in enumerate( open( ipListFile ), start = 1 ):
    line = line.strip()

    if line == "" or line.startswith( "#" ):
        continue

    family, cidr = normalize( line.split()[ 0 ], lineNumber )

    if cidr not in seen:
        seen.add( cidr )
        requested[ family ].append( cidr )

if not seen:
    fail( "the ip list file contains no addresses" )


inbound = current.get( "inbound", [] ) or []

def isOwned( rule ):
    return str( rule.get( "label", "" ) ).startswith( labelPrefix + "_" )

ownedRules = [ r for r in inbound if isOwned( r ) ]
otherRules = [ r for r in inbound if not isOwned( r ) ]

# Everything already inside our own rules, so that a re-run is a no-op rather
# than a second copy of the same addresses.
alreadyBlocked = { "ipv4": [], "ipv6": [] }
alreadySeen = set()

for rule in ownedRules:
    for family in ( "ipv4", "ipv6" ):
        for cidr in ( rule.get( "addresses", {} ) or {} ).get( family, [] ) or []:
            if cidr not in alreadySeen:
                alreadySeen.add( cidr )
                alreadyBlocked[ family ].append( cidr )

# Addresses a rule we do not own would already have dropped are still worth
# adding to ours -- that rule could be edited or removed later -- so the only
# thing we skip is what is already in our own rules.
toAdd = { f: [ c for c in requested[ f ] if c not in alreadySeen ]
          for f in ( "ipv4", "ipv6" ) }

addedCount = len( toAdd[ "ipv4" ] ) + len( toAdd[ "ipv6" ] )
skippedCount = ( len( requested[ "ipv4" ] ) + len( requested[ "ipv6" ] )
                 - addedCount )

# The full owned set: what we had, plus what is new.  Rebuilt from scratch
# every run, which is what keeps the chunks densely packed.
finalAddresses = ( [ ( "ipv4", c ) for c in
                     alreadyBlocked[ "ipv4" ] + toAdd[ "ipv4" ] ]
                   + [ ( "ipv6", c ) for c in
                       alreadyBlocked[ "ipv6" ] + toAdd[ "ipv6" ] ] )

chunks = [ finalAddresses[ i : i + maxAddresses ]
           for i in range( 0, len( finalAddresses ), maxAddresses ) ]

if len( chunks ) + len( otherRules ) > maxRules:
    fail( "this needs %d rules but the firewall allows %d in total, and %d "
          "are already used by rules this script does not manage.\n"
          "       At %d addresses per rule that is a ceiling of %d addresses.\n"
          "       Collapse addresses into CIDR ranges, or use a second "
          "firewall."
          % ( len( chunks ) + len( otherRules ), maxRules, len( otherRules ),
              maxAddresses, ( maxRules - len( otherRules ) ) * maxAddresses ) )

newOwnedRules = []

for number, chunk in enumerate( chunks, start = 1 ):
    addresses = { "ipv4": [ c for f, c in chunk if f == "ipv4" ],
                  "ipv6": [ c for f, c in chunk if f == "ipv6" ] }

    newOwnedRules.append( {
        "action": "DROP",
        "label": "%s_%03d" % ( labelPrefix, number ),
        "description": "blocked by %s" % labelPrefix,
        "protocol": "TCP",
        "ports": "1-65535",
        "addresses": addresses } )

# Owned rules first: Linode stops at the first matching rule, so a DROP that
# sat behind an existing ACCEPT would never be consulted.
merged = dict( current )
merged[ "inbound" ] = newOwnedRules + otherRules

# version and fingerprint come back from the GET but are not accepted on a
# write, and the policies are carried through untouched.
merged.pop( "version", None )
merged.pop( "fingerprint", None )
merged.setdefault( "outbound", current.get( "outbound", [] ) or [] )
merged.setdefault( "inbound_policy", "ACCEPT" )
merged.setdefault( "outbound_policy", "ACCEPT" )

json.dump( merged, open( outputFile, "w" ) )


out = sys.stderr.write

out( "firewall rules now: %d TCP DROP rule(s) holding %d address(es)\n"
     % ( len( newOwnedRules ), len( finalAddresses ) ) )
out( "  %d address(es) newly blocked\n" % addedCount )

if skippedCount:
    out( "  %d already blocked by this script, left alone\n" % skippedCount )

out( "  %d rule(s) left untouched\n" % len( otherRules ) )
out( "  using %d of the %d rules this firewall allows\n"
     % ( len( newOwnedRules ) + len( otherRules ), maxRules ) )

if current.get( "inbound_policy" ) == "DROP" and not otherRules:
    out( "\nnote: this firewall's inbound policy is already DROP with no "
         "other\n      inbound rules, so everything is blocked anyway and "
         "these rules\n      change nothing in practice.\n" )

if addedCount == 0:
    out( "\nnothing to do, every address is already blocked\n" )
    sys.exit( 2 )
PYTHON_END

mergeStatus=$?

if [ $mergeStatus -eq 2 ]; then
    # Nothing new to send.  Not a failure.
    exit 0
fi

if [ $mergeStatus -ne 0 ]; then
    exit 1
fi


if [ "$dryRun" = "--dry-run" ]; then
    echo ""
    echo "dry run, firewall $firewallID was not changed"
    exit 0
fi


httpStatus=$( curl -s -o "$workDir/response.json" -w '%{http_code}' \
                   -X PUT \
                   -H @"$headerFile" \
                   -H "Content-Type: application/json" \
                   --data-binary @"$workDir/newRules.json" \
                   "$apiURL" )

if [ "$httpStatus" != "200" ]; then
    echo "" >&2
    echo "$scriptName: firewall update failed (HTTP $httpStatus)" >&2
    head -c 1000 "$workDir/response.json" >&2
    echo >&2
    echo "the firewall was left unchanged" >&2
    exit 1
fi

echo ""
echo "firewall $firewallID updated"
