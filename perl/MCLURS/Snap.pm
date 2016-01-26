#!/usr/bin/perl -w

package MCLURS::Snap;

use strict;

BEGIN {
    $ENV{ PERL_ZMQ_BACKEND } = 'ZMQ::LibZMQ3';
}

use ZMQ;
use ZMQ::Constants qw(:all);
use ZMQ::Poller;

# Constructor

sub new {
    my $snap = bless({}, shift);
    if( $snap->_init(@_) ) {
	return $snap;
    }
    return undef;
}

# Initialiser

my @req = qw( skt );

sub _init {
    my $self = shift;
    my %args = @_;

    @{$self}{ keys %args } = ( values %args );
    for my $a ( @req ) {
	next if( defined $self->{$a} );
	# Error -- missing mandatory element
	return;
    }
    unless( ref($self->{skt}) =~ /ZMQ::Socket/ ) {
	# Error -- $skt should be a ZMQ socket
	return;
    }

    $self->{_estr} = '';
    $self->{_snap} = $self->{skt}->getsocopt(ZMQ_LAST_ENDPOINT);
    unless( $self->{_snap} ) {
	# Error -- $skt is not connected
	$self->{_estr} = "Socket is not connected";
	return 1;
    }
    
    my $p = ZMQ::Poller->new();
    unless($p) {
	$self->{_estr} = "Could not create Poller";
	return 1;
    }

    $self->{_poll} = $p;
    $p->register($self->{skt}, ZMQ_POLLOUT|ZMQ_POLLIN);
    $self->{timeout} ||= 1000;
    
    # Successful, return true
    return 1;
}

# Non-blocking read from the socket
sub _read {
    my $self = shift;

    my $s = $self->_waitr(0);
    return unless($s);

    my $m = $s->recvmsg(0);
    unless($m) {
	$self->{_estr} = "Read failed: $!";
	return;
    }
    
    my $d = $m->data();
    $m->close();
    $self->{_estr} = '';
    chomp $d;
    return $d;
}

# Non-blocking write to the socket
sub _write {
    my $self = shift;

    my $s = $self->_waitw(0);
    return unless($s);

    my $m = ZMQ::Message->new(join('', @_));
    my $ret = $s->send($m, $m->size());

    unless($ret == $m->size()) {
	$self->{_estr} = "Write failed: $!";
	return;
    }
    
    $m->close();
    $self->{_estr} = '';
    return 1;
}

# Wait until socket is readable; arg is wait (poll) time in [ms]
sub _waitr {
    my $self = shift;
    my $time = $_[0];
    $time = $self->{timeout} || 1000 unless(defined($time));

    my ($s) = $self->{_poll}->poll($time);
    unless($s and $s->{events}&ZMQ_POLLIN != 0) {
	$self->{_estr} = "Poll Timeout on Read";
	return;
    }
    return $self->{skt};
}

# Wait until socket is writable; arg is wait (poll) time in [ms]
sub _waitw {
    my $self = shift;
    my $time = $_[0];
    $time = $self->{timeout} || 1000 unless(defined($time));

    my ($s) = $self->{_poll}->poll($time);
    unless($s and $s->{events}&ZMQ_POLLOUT != 0) {
	$self->{_estr} = "Poll Timeout on Write";
	return;
    }
    return $self->{skt};
}

# Execute a transaction (send/receive) with snapshotter
sub _transact {
    my $self = shift;
    my %params = @_;

    # For a transaction, 
    # first wait until the socket is writable
    return unless( $self->_waitw( $params{timeout} ) );
    # next, write the message to the socket
    return unless( $self->_write( $params{cmd} ) );
    # then wait until the socket is readable
    return unless( $self->_waitr( $params{timeout} ) );
    # then read and interpret the reply
    my $r = $self->_read();
    return unless($r);
    
    $self->{_reply} = $r;

    if( $r !~ /^((?:OK|NO\:))\s+(.*)$/ ) {
	$self->{_estr} = "Reply format unexpected";
	return;
    }

    if( $1 eq 'OK' ) {
	$self->{_estr} = '';
	$self->{_reply} = $2;
	return 1;
    } else {
	$self->{_estr} = $2;
	$self->{_reply} = '';
	return 0;
    }
}

# Query whether an error has occurred
sub error {
    my $self = shift;
    return $self->{_estr};
}

# Get reply string from a transaction
sub reply {
    my $self = shift;
    return $self->{_reply};
}

# Instruct snapshotter to quit
sub quit {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Quit command when not running";
	return;
    }
    return $self->_transact(cmd => 'Q', timeout => 3000);
}


sub DESTROY {
    my $self = shift;
    $self->{_poll}->unregister($self->{skt});
}

# Evaluate true as final step
1;

