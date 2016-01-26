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
    $self->{_snap} = $self->{skt}->getsockopt(ZMQ_LAST_ENDPOINT);
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

    # Check any parameters supplied on creation
    $self->_paramcheck();

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

#    print STDERR "Using socket $s\n";
    my $m = ZMQ::Message->new(join('', @_));
    my $ret = $s->sendmsg($m, 0);
#    print STDERR "Message send returns $ret\n";

    unless($ret >= 0) {
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
#    print STDERR "Step 1\n";
    return unless( $self->_waitw( $params{timeout} ) );
    # next, write the message to the socket
#    print STDERR "Step 2\n";
    return unless( $self->_write( $params{cmd} ) );
    # then wait until the socket is readable
#    print STDERR "Step 3\n";
    return unless( $self->_waitr( $params{timeout} ) );
    # then read and interpret the reply
#    print STDERR "Step 4\n";
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

# Check parameter formats

my %Params = ( freq	=> qr/[\d\.]+/,
	       range 	=> qr/500|750/,
	       bufsz 	=> qr/\d+/,
	       window 	=> qr/[\d\.]+/,
	       sscorr 	=> qr/-?(1|0.\d+)/,
	       bufhwm	=> qr/0\.\d+/,
    );

sub _paramcheck {
    my $self = shift;

    for my $p ( keys %Params ) {
	if( defined($self->{$p}) ) {
	    my $v = $Params{$p};
	    
	    if( ref($v) =~ /CODE/i ) {
		&{$v}($self, $p);
	    } else {
		$self->{_estr} .= "Bad '$p' parameter value '$self->{$p}'\n" if( $self->{$p} !~ m/^$v$/ );
	    }
	}
    }
    if( $self->{_estr} ) {
	chomp( $self->{_estr} );
	return;
    }
    return 1;
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

# Set parameters for snapshotter
sub set {
    my $self = shift;
    my %params = @_;

    $self->{_estr} = '';
    for my $p (keys %params) {
	$self->{_estr} .= "Unknown parameter '$p'\n", next unless( defined $Params{$p} );
	$self->{$p} = $params{$p};
    }
    return $self->_paramcheck();
}

# Retrieve the parameters from the snapshotter object
sub params {
    my $self = shift;
    my @plist = qw( freq sfreq bufsz bufhwm window sscorr isp nchan state dir );
    return { map { defined $self->{$_}? ($_, $self->{$_}) : (); } @plist };
}

# Instruct snapshotter to start
sub start {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Go command when not running";
	return;
    }

    $self->_transact(cmd => 'go');
    return if( $self->{_estr} );
    $self->{state} = 'armed';
    return 1;
}

# Instruct a snapshotter to stop
sub stop {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Halt command when not running";
	return;
    }

    $self->_transact(cmd => 'halt');
    return if( $self->{_estr} );
    $self->{state} = 'halt';
    for my $p ( qw(nchan isp sfreq) ) { 
	delete $self->{$p};
    }
    return 1;    
}

# Instruct snapshotter to quit
sub quit {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Quit command when not running";
	return;
    }

    $self->_transact(cmd => 'Q', timeout => 3000);
    return if( $self->{_estr} );
    for my $p ( qw(nchan isp sfreq dir) ) { 
	delete $self->{$p};
    }
    $self->{_run} = 0;
    return 1;
}

# Collect snapshotter status
sub status {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Ztatus command when not running";
	return;
    }
    return $self->_transact(cmd => 'Z');
}

# Set up snapshotter: ends in init state
sub setup {
    my $self = shift;

    # First, get status, check if running
    unless( $self->_transact(cmd => 'z') ) {
	$self->{_estr} = "Cannot talk to snapshotter";
	$self->{_run} = 0;
	return;
    }
    $self->{_run} = 1;
    if( $self->{_reply} !~ m/READER\s+(\w+)/) {
	$self->{_estr} = "Zstatus reply format unexpected";
	return;
    }
    $self->{state} = lc($1);

    # Check snapshotter is not already collecting data
    if( $self->{state} eq 'armed' || $self->{state} eq 'run' ) {
	$self->{_estr} = "Cannot set up snapshot during capture";
	return;
    }

    # Next, set all parameters from local structure
    my $pcmd = '';
    for my $p (sort keys %Params) {
	$pcmd .= "$p=$self->{$p}, " if( defined $self->{$p} );
    }
    $pcmd =~ s/,\s+$//;
    unless( $self->_transact(cmd => "P $pcmd") ) {
	return;
    };

    # Now execute Init command
    # Expect reply like 'Init -- nchan 8 isp 400[ns]'
    unless( $self->_transact(cmd => "I") ) {
	return;
    }
    if( $self->reply() !~ m/nchan\s+(\d+)\s+isp\s+(\d+)/ ) {
	$self->{_estr} = "Init reply format unexpected";
	$self->{state} = 'init';
	return;
    }

    # Next, store the data from the Init reply
    $self->{nchan} = $1;
    $self->{isp}   = $2;
    $self->{state} = 'init';
    $self->{sfreq}  = int(0.5 + 1e9/$self->{isp});

    # Last, reset the writer's working directory
    unless( $self->_transact(cmd => 'd') ) {
	return;
    }
    $self->{dir} = '';

    # All done
    return 1;
}

# Request a snapshot from the snapshotter

my @SnapParams = qw(begin end start finish length count path);

sub snap {
    my $self = shift;
    my %args = @_;
    my %SP;

    unless( $self->{_run} ) {
	$self->{_estr} = "Snap command when not running";
	return;
    }

    unless( $self->{state} eq 'armed' || $self->{state} eq 'run' ) {
	$self->{_estr} = "Snap command before capture started";
	return;
    }

    my $sargs = '';
    @SP{@SnapParams} = ();
    for my $a ( keys %args ) {
	$sargs .= "$a=$args{$a}, " if( exists $SP{$a} );
    }
    $sargs =~ s/,\s+$//;

    $self->_transact(cmd => "S $sargs");
    return if( $self->{_estr} );
    if( $self->reply() !~ m/Snap ([0-9a-fA-F]+)/ ) {
	$self->{_estr} = "Snap reply format unexpected";
	return;
    }
    return "S" . lc($1);
}

# Set/get the snapshotter's working directory
sub dir {
    my $self = shift;
    my $dir  = shift;

    # Instruct the snapshotter to change directory if necessary
    if(defined $dir) {
	return 1 if($dir eq $self->{dir});

	unless( $self->{_run} ) {
	    $self->{_estr} = "Dir set command when not running";
	    return;
	}
	$self->{dir} = '';
	$self->_transact(cmd => "D path=$dir");
	return if( $self->{_estr} );
	$self->{dir} = $dir;
    }
    return $self->{dir};
}

sub DESTROY {
    my $self = shift;

    $self->{_poll}->unregister($self->{skt});
}

# Evaluate true as final step
1;

