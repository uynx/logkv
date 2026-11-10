{ pkgs, ... }:
{
  packages = [ pkgs.gcc pkgs.gnumake pkgs.fio pkgs.jq ];
}
