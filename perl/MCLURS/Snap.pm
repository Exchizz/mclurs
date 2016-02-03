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
	$snap->{_busy} = 0;
	return $snap;
    }
    return undef;
}

# Initialiser
#
# ARG includes mandatory skt parameter (the socket to use) plus any others.

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
    $self->{_busy} = 0;
    
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
    $self->{_busy} = 1;
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

# Translate snapshot status codes -- see chunk.h, chunk.c in snapshotter
my %snapshot_state =(
    'INI' => 'ini',
    'ERR' => 'err',
    'PRP' => 'prp',
    'RDY' => 'rdy',
    '...' => 'inQ',
    '>>>' => 'wri',
    '+++' => 'wtn',
    'FIN' => 'fin',
    );

# Parse a snapshot status line
sub _parse_snapshot_status {
    my $self = shift;
    local $_;

    $_ = shift;

    # Completed snapshot
    if( m!^OK\s+Snap\s+(?:[Ss]:)?([\dA-Fa-f]+):\s+FIN\s+(\d+)/(\d+)\s+files$! ) {
	my ($sn,$done,$count) = ("s:" . lc($1), $2, $3);

	my $snap = $self->{_snapshots}->{$sn};
	unless( $snap ) {
	    $self->{_estr} = "Cannot locate history for snapshot $sn";
	    return;
	}

	unless( $count == 1 || $count == $snap->{count} ) {
	    $self->{_estr} = "Finished snapshot $sn has inconsistent count $count vs. $snap->{count}";
	    return;
	}
	$snap->{count}   = $count;
	$snap->{done}    = $done;
	$snap->{state}   = 'fin';
	$snap->{pending} = 0;
	return 1;
    }

    # In progress snapshot
    if( m!^Snap\s+(?:[Ss]:)?([\dA-Fa-f]+):\s+([^\s]+)\s+(\d+)/(\d+)/(\d+)$! ) {
	my $sn = "s:" . lc($1);

	my $snap = $self->{_snapshots}->{$sn};
	unless( $snap ) {
	    $self->{_estr} = "Cannot locate history for snapshot $sn";
	    return;
	}

	$snap->{state}   = $snapshot_state{$2};
	my ($done,$pending,$count) = ($3,$4,$5);
	unless( $count == 1 || $count == $snap->{count} ) {
	    $self->{_estr} = "Snapshot $sn has inconsistent count $count vs. $snap->{count}";
	    return;
	}
	$snap->{count}   = $count;
	$snap->{done}    = $done;
	$snap->{pending} = $pending;
	return 1;
    }

    # Failed snapshot
    if( m!^NO:\s+Snap\s+(?:[Ss]:)?([\dA-Fa-f]+)\s+(.*)$! ) {
	my $sn = "s:" . lc($1);

	my $snap = $self->{_snapshots}->{$sn};
	unless( $snap ) {
	    $self->{_estr} = "Cannot locate history for snapshot $sn";
	    return;
	}

	$snap->{state} = 'err';
	$snap->{emsg}  = $2;
	return 1;
    }

    # Unknown reply line
    $self->{_estr} = "Cannot parse status reply line";
    return;
}

# Process a reply from the Ztatus command to catch snapshot status
sub _do_status_reply {
    my $self = shift;
    local $_;

    $_ = $self->{_reply};
    return unless( m/Ztatus/ );

    # Split after every (internal) newline
    chomp;
    my ($l1, @lines) = split /\r?\n/, $_;

    # Running snapshotter always supplies at least 1 status line
    unless($l1) {
	$self->{_estr} = "Zstatus reply line missing";
	return;
    }
    
#    print STDERR "Zstatus reply has ", scalar(@lines), " lines\n";

    # Process the first (general) status line
    $self->{state} = lc($1) if( $l1 =~ m/READER\s+(\w+)/ );
    return 1 unless(@lines);

    # Process the pending-snapshot reports
    my $n = 2;
    for my $l ( @lines ) {
	return unless( $self->_parse_snapshot_status($l) );
	$n++;
    }
    return 1;
}

###
### Public methods
###

### These all return true for success and undef for error.  Error
### information can be retrieved using the error() method provided no
### other calls on the object have been run.  Successful methods
### return 1 unless otherwise noted.

# Query whether the object is busy -- i.e. has a message been written
# but a reply not yet received.
#
# Returns the value of the _busy variable.

sub busy {
    my $self = shift;
    return $self->{_busy};
}

# Query whether an error has occurred; in some cases (e.g. parse
# errors the reply() method can also return useful information).
#
# Return the current content of the _estr slot.

sub error {
    my $self = shift;
    return $self->{_estr};
}

# Get reply string from a transaction
#
# Return the current content of the _reply slot.

sub reply {
    my $self = shift;
    return $self->{_reply};
}

# Set parameters for snapshotter
#
# ARG is a hash giving parameters to set; the parameters are those
# that can be set at run time in the snapshotter -- see snapshotter(8).

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
#
# Result: a reference to a hash containing the parameters supplied by
# set() plus extra parameters collected or inferrred from snapshotter
# responses.

sub params {
    my $self = shift;
    my @plist = qw( freq sfreq bufsz bufhwm window sscorr isp nchan state dir );
    return { map { defined $self->{$_}? ($_, $self->{$_}) : (); } @plist };
}

# Instruct snapshotter to start
#
# No arguments.  Assumes snapshotter is initialised (init state).

sub start {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Go command when not running";
	return;
    }

    $self->_transact(cmd => 'go');
    return if( $self->{_estr} );
    $self->{state} = 'armed';
    $self->{_snapshots} = {};	# Create the snapshot status hash
    return 1;
}

# Instruct a snapshotter to stop
#
# No arguments.  Assumes snapshotter is capturing (armed or run state).

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
#
# No arguments;  sends Q command to snapshotter and tidies up.

sub quit {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Quit command when not running";
	return;
    }

    $self->_transact(cmd => 'Q', timeout => 3000);
    return if( $self->{_estr} );
    for my $p ( qw(nchan isp sfreq dir _snapshots ) ) { 
	delete $self->{$p};
    }
    $self->{_run} = 0;
    return 1;
}

# Collect snapshotter status
#
# No arguments.  Sends a Z command and interprets the response.

sub update {
    my $self = shift;

    unless( $self->{_run} ) {
	$self->{_estr} = "Ztatus command when not running";
	return;
    }
    $self->_transact(cmd => 'Z');
    return $self->_do_status_reply();
}

# Probe to check that the snapshotter is running
#
# No arguments
#
# If the object is not busy, attempt a z command.  If that succeeds, set _run.
# If the transaction times out, then _busy should be set, in which case return
# without setting the error string.
#
# If the object is busy, that means no one was listening last time;
# maybe they are now.  Therefore attempt a reset.  Set _run if it works.

sub probe {
    my $self = shift;

    # Are we busy?  If so, see if the reply is here yet
    if( $self->{_busy} ) {
	my $s = $self->reset();
	$self->{_run} = 1 if($s);
	return $s;
    }

    # Not busy, so try a status command
    unless( $self->_transact(cmd => 'z') ) {
	$self->{_run} = 0;
	return if( $self->{_busy} ); # Waiting for reply

	$self->{_estr} = "Cannot talk to snapshotter";
	return;
    }

    # Status succeeded, so other end is running
    $self->{_run} = 1;
    return $self->_do_status_reply();
}

# Reset the communications after a failure
#
# No arguments

sub reset {
    my $self = shift;

    return 1 unless( $self->{_busy} );
    $self->_read();		# Read anything that is pending
    return $self->_waitw(0);	# Check if we can now write
}

# Set up snapshotter: ends in init state
#
# Initialise the snapshotter.  Sends specified parameters, then init
# command, then resets the snapshot working directory.

sub setup {
    my $self = shift;

    return unless( $self->probe() );

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
#
# Argument is a hash containing at least the necessary parameters to
# cause a snapshot to be attempted.  Other parameters (key-value
# pairs) are stored locally but not sent to the snapshotter.
#
# Returns the snapshot identifier on success.

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

    # Copy the _necessary_ parameters; the %arg hash may contain
    # other key-value pairs that the caller wishes to associate with
    # the snapshot, and they will be preserved in the history entry.

    my $sargs = '';
    @SP{@SnapParams} = ();
    for my $a ( keys %args ) {
	$sargs .= "$a=$args{$a}, " if( exists $SP{$a} );
    }
    $sargs =~ s/,\s+$//;

    # Request the snapshot from the snapshotter
        
    $self->_transact(cmd => "S $sargs");
    return if( $self->{_estr} );
    if( $self->reply() !~ m/Snap (?:[Ss]:)?([0-9a-fA-F]+)/ ) {
	$self->{_estr} = "Snap reply format unexpected";
	return;
    }

    # NOT WITH THE CODE BELOW; IT DOESN'T
    # Note re-use of %SP here: this copies all the args into the
    # snapshot history entry, and leaves any parameters not set by
    # the %arg hash present as keys but with undefined value.
    #
    # The snapshot history entry is stored under the snapshotter's
    # snapshot name (hex uid) prefixed with 's:'.  Subsequent Zstatus
    # replies refresh and update the history (i.e. the 'state').

    my $snap = "s:" . lc($1);
    %SP = ( %args, name => $snap, state => 'snd' );
    $self->{_snapshots}->{$snap} = \%SP;
    return $snap;
}

# Set/get the snapshotter's working directory
#
# ARG is a path => set the dir locally and in the snapshotter
# ARG unset => do not change the snapshotter's working directory
#
# Returns the old working directory on success.

sub dir {
    my $self = shift;
    my $dir  = shift;
    my $old  = $self->{dir} || '.';

    # Instruct the snapshotter to change directory if necessary
    if(defined $dir) {
	return $old if($dir eq $self->{dir});

	unless( $self->{_run} ) {
	    $self->{_estr} = "Dir set command when not running";
	    return;
	}
	$self->{dir} = '';
	$self->_transact(cmd => "D path=$dir");
	return if( $self->{_estr} );
	$self->{dir} = $dir;
    }
    return $old;
}

# Report the status of one or all snapshots
#
# ARG (optional) a snapshot name
#
# Returns on success a (copy of the) snapshot descriptor hash if a
# name was given or a list of such hashes if no name was provided.
# Always returns a list in list context, may sensibly return a scalar
# if given a name in a scalar context call.

sub status {
    my $self = shift;
    my $name = shift;

    $self->{_reply} = '';
    $self->{_estr}  = '';
    if( defined $name ) {
	my $snap = $self->{_snapshots}->{$name};
	unless( $snap ) {
	    $self->{_estr} = "Cannot locate history for snapshot $name";
	    return;
	}
	my $ans = { %{$snap} };
	return wantarray? ( $ans ) : $ans;
    }
    return ( map {
	( { %{$self->{_snapshots}->{$_}} } );
	     } keys %{$self->{_snapshots}}
    );
}

# Clear a snapshot's status history
#
# ARG (mandatory) the snapshot name
#
# Returns 1 for successfully completed snapshot.
# If snapshot failed, returns 0 and leaves snapshot error message in _estr.

sub clear {
    my $self = shift;
    my $name = shift;

    $self->{_reply} = '';
    $self->{_estr}  = '';
    unless( $name ) {
	$self->{_estr} = "Clear command without named snapshot";
	return;
    }

    my $snap = $self->{_snapshots}->{$name};
    unless( $snap->{state} eq 'fin' or $snap->{state} eq 'err' ) {
	$self->{_estr} = "Clear of unfinished snapshot $name";
	return;
    }
    delete $self->{_snapshots}->{$name};
    return 1 if( $snap->{state} eq 'fin' );
    $self->{_estr} = $snap->{emsg};
    return 0;
}

# Evaluate true as final step
1;

