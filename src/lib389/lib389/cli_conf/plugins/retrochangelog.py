# --- BEGIN COPYRIGHT BLOCK ---
# Copyright (C) 2019 Red Hat, Inc.
# All rights reserved.
#
# License: GPL (version 3 or any later version).
# See LICENSE for details.
# --- END COPYRIGHT BLOCK ---

from lib389.plugins import RetroChangelogPlugin
from lib389.cli_conf import add_generic_plugin_parsers, generic_object_edit

arg_to_attr = {
    'is-replicated': 'isReplicated',
    'attribute': 'nsslapd-attribute',
    'directory': 'nsslapd-changelogdir',
    'max-age': 'nsslapd-changelogmaxage',
}


def retrochangelog_edit(inst, basedn, log, args):
    log = log.getChild('retrochangelog_edit')
    plugin = RetroChangelogPlugin(inst)
    generic_object_edit(plugin, log, args, arg_to_attr)


def _add_parser_args(parser):
    parser.add_argument('--is-replicated', choices=['true', 'false'],
                        help='Sets a flag to indicate on a change in the changelog whether the change is newly made '
                             'on that server or whether it was replicated over from another server (isReplicated)')
    parser.add_argument('--attribute',
                        help='Specifies another Directory Server attribute which must be included in '
                             'the retro changelog entries (nsslapd-attribute)')
    parser.add_argument('--directory',
                        help='Specifies the name of the directory in which the changelog database '
                             'is created the first time the plug-in is run')
    parser.add_argument('--max-age',
                        help='This attribute specifies the maximum age of any entry '
                             'in the changelog (nsslapd-changelogmaxage)')


def create_parser(subparsers):
    retrochangelog = subparsers.add_parser('retro-changelog', help='Manage and configure Retro Changelog plugin')
    subcommands = retrochangelog.add_subparsers(help='action')
    add_generic_plugin_parsers(subcommands, RetroChangelogPlugin)

    edit = subcommands.add_parser('set', help='Edit the plugin')
    edit.set_defaults(func=retrochangelog_edit)
    _add_parser_args(edit)


