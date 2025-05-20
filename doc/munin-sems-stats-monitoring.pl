#!/usr/bin/perl

use strict;
use warnings;

if($ARGV[0] and $ARGV[0] eq 'config') {
        print "graph_title SEMS calls\n";
        print "graph_args --base 1000 -l 0\n";
        print "graph_vlabel calls\n";
        print "graph_category Porting\n";

	print "calls.draw LINE2\n";
	print "calls.label current\n";
	print "peak.draw LINE2\n";
	print "peak.label peak calls\n";
        exit 0;
}

open my $fh_active, '-|', 'sems-stats';

while (<$fh_active>)
{
	if($_ =~ /Active calls: (.*)\n/)
	{
		print "calls.value $1\n";
	}
}
close $fh_active;

open my $fh_max, '-|', 'sems-stats -c get_callsmax';

while (<$fh_max>)
{
	if($_ =~ /Maximum active calls: (.*)\n/)
	{
		print "peak.value $1\n";
	}
}
close $fh_max;
