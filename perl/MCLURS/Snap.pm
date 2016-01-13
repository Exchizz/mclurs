#!/usr/bin/perl -w

package MCLURS::Snap;

use strict;

# Constructor

sub new {
    my $snap = bless({}, shift);
    if( $snap->_init(@_) ) {
	return $snap;
    }
    return undef;
}

# Initialiser

my @req = qw(ctxt, addr);

sub _init {
    my $self = shift;
    my %args = @_;

    @{$self}{ keys %args } = ( values %args );
    for my $a ( @req ) {
	next if( defined $self->{$a} );
	# Error -- missing mandatory element
	return;
    }

    # Successful, return true
    return 1;
}



# Evaluate true as final step
1;

