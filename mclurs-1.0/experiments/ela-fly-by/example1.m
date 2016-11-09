clc; clear all; close all;
%% Setup of the environment
addpath( genpath( 'Hardware_Interface\') )
addpath( genpath( 'Signal_Processing') )

%% Setup of the sonar parameters:
% Load the mic coordinates
load( 'PCB_Sonar_Mics' );

Fs_ADC = 312500;
Fs_DAC = 250000;

%% Setup of the used call:
duration = 2; % Duration in msec
sig_DAC = fm_sweep( 100000, 25000, Fs_DAC, duration, 1, 10 );
sig_ADC = fm_sweep( 100000, 25000, Fs_ADC, duration, 1, 10 );
used_base_sig = sig_ADC;

FTDI_uploadCall( sig_DAC );
[ b_lp, a_lp ] = butter( 2, 2000 / ( Fs_ADC / 2 ) );
[ b_bp, a_bp ] = butter( 6, [ 30000 60000 ] / ( Fs_ADC / 2 ) );

%% Setup Escape Generation:
% The Escape Settings:
Escape_Subs_Fact = 5;  % Subsample factor
R_min = 1.4; % Minimal distance
R_max = 4;  % Maximal distance
az_vec = -45 : 1 : 45; % The desired azimuths for the angle vector
el_vec = ones( size( az_vec ) ) * 0; % The desired elevations for the angle vector

% Crunch some numbers
spl_start = round( R_min * 2 / 343 * Fs_ADC ); % The first sample
spl_stop = round( R_max * 2 / 343 * Fs_ADC ) ; % The last sample
n_R_pts = ceil( ( spl_stop - spl_start + 1 ) / Escape_Subs_Fact );
R_vec = linspace( R_min, R_max, n_R_pts ); % A vector with all the ranges for each ES-voxel
[ delay_matrix_azslice, dir_coordinates_azslice ] = azel_2_delayints( az_vec, el_vec, mic_coordinates, Fs_ADC );
n_az_pts = length( az_vec );
R_mat_az =  repmat( R_vec', 1, n_az_pts ); % A matrix with the ranges for all the ES_voxels (same size as ES);
az_mat = ( repmat( az_vec, n_R_pts, 1 ) ); % A matrix with all the azimuths for the ES voxels

%% Some aditional parameters:
settings.delay_matrix = delay_matrix_azslice;
settings.base_sig = sig_ADC;
settings.a_lp = a_lp;
settings.b_lp = b_lp;

weights_repped = repmat( weights, 1, spl_stop - spl_start + 1 );

%% Data capture and basic data cleanup:
close all
%start recordings
setgain(160)
dt_2=datestr(now,'mmmm-dd-HH-MM-SS');
stop_sampling();
pause(.5);
start_sampling(dt_2,10);  %cal iwth filename and duration (10s) of recording (per trial)
%issue delay
pause(0.1) %100ms delay

for k=1:10
    dt = datestr(now,'mmmm-dd-HH-MM-SS-FFF');
    
    % emit Signal and capture a sonar measurement
    data1 = FTDI_GetData( Fs_ADC, 1 );
    
    % Preprocess the signals
    data = rdc( data1  );
    data = clean_sig_cut( data, spl_start, spl_stop );
    data = data .* weights_repped;
    data = filter( b_bp, a_bp, rdc( data ), [], 2 );
    
    % Calculate the energyscape and do basic cleanup
    [ E_scape ] = fast_SpatioTemp_MF( data, settings );
    E_scape_subs = E_scape( 1 : Escape_Subs_Fact : length( data ) ,  : );
    E_scape_subs = E_scape_subs  - repmat( mean( E_scape_subs, 2 ), 1, size( E_scape_subs, 2 ) ) ;
    E_scape_subs = E_scape_subs  - repmat( mean( E_scape_subs, 1 ), size( E_scape_subs, 1 ), 1 ) ;
    E_scape_subs( E_scape_subs < 0 ) = 0;
    E_scape_subs = E_scape_subs .* ( R_mat_az .^ 0.5 );
    E_scape_subs = E_scape_subs / 5e4;
    
    % saving data
    %     data_structure.data=data;
    %     data_structure.raw_data=data1;
    %     data_structure.signal_ADC=sig_ADC;
    %     data_structure.signal_DAC=sig_DAC;
    %     data_structure.fs_ADC=Fs_ADC;
    %     data_structure.fs_DAC=Fs_DAC;
    filename  = '05202016_LDRD_drone_rightward_19.mat';
    %     save(dt,'data_structure');
    save(dt);
    
    % Show the results
    figure( 1 );
    clf
    cla
    imagesc( az_vec, R_vec, E_scape_subs );
    set( gca, 'YDir', 'normal' );
    colormap hot;
    colorbar
end