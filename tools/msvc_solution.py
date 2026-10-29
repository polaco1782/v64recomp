#!/usr/bin/env python3
"""Write a browsable MSVC project per game and one shared Recomp project."""
from __future__ import annotations

import argparse
from pathlib import Path
import tomllib
import uuid
import xml.etree.ElementTree as ET

NS = 'http://schemas.microsoft.com/developer/msbuild/2003'
CPP_PROJECT = '{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}'
SOLUTION_FOLDER = '{2150E333-8FDC-42A3-9474-1A3956D46DE8}'
ET.register_namespace('', NS)


def guid(name: str) -> str:
    return '{' + str(uuid.uuid5(uuid.NAMESPACE_URL, 'v64recomp/msvc/' + name)).upper() + '}'


def element(parent, name, text=None, **attributes):
    item = ET.SubElement(parent, '{' + NS + '}' + name, attributes)
    if text is not None:
        item.text = text
    return item


def write_xml(path: Path, document):
    ET.indent(document, space='  ')
    path.parent.mkdir(parents=True, exist_ok=True)
    text = ET.tostring(document, encoding='unicode', xml_declaration=False)
    path.write_text('<?xml version="1.0" encoding="utf-8"?>\n' + text + '\n', encoding='utf-8', newline='\r\n')


def source_files(root: Path, game: str) -> list[Path]:
    extensions = {'.c', '.cc', '.cpp', '.h', '.hpp', '.inl', '.py', '.toml', '.json', '.cmake', '.ps1', '.sh'}
    directories = [root / 'games' / game] if game != 'none' else [root / p for p in ('runtime', 'include', 'backends', 'tools', 'cmake')]
    files = {p for directory in directories for p in directory.rglob('*')
             if p.is_file() and (p.suffix in extensions or p.name == 'CMakeLists.txt')
             and '__pycache__' not in p.parts}
    if game == 'none':
        files.update(root / p for p in ('CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json',
                                       'requirements.txt', 'msvc/Recomp.props'))
    return sorted(files)


def write_project(root: Path, game: str) -> Path:
    name = 'Recomp' if game == 'none' else game
    path = root / 'msvc' / ('recomp.vcxproj' if game == 'none' else f'games/{game}.vcxproj')
    project = ET.Element('{' + NS + '}Project', DefaultTargets='Build', ToolsVersion='Current')
    configurations = element(project, 'ItemGroup', Label='ProjectConfigurations')
    for mode in ('Release', 'Debug'):
        item = element(configurations, 'ProjectConfiguration', Include=mode + '|x64')
        element(item, 'Configuration', mode)
        element(item, 'Platform', 'x64')
    globals = element(project, 'PropertyGroup', Label='Globals')
    element(globals, 'ProjectGuid', guid(name))
    element(globals, 'Keyword', 'MakeFileProj')
    element(globals, 'ProjectName', name)
    element(globals, 'RecompGame', game)
    element(project, 'Import', Project=r'$(VCTargetsPath)\Microsoft.Cpp.Default.props')
    config = element(project, 'PropertyGroup', Label='Configuration')
    element(config, 'ConfigurationType', 'Makefile')
    element(config, 'PlatformToolset', '$(DefaultPlatformToolset)')
    element(project, 'Import', Project=r'$(VCTargetsPath)\Microsoft.Cpp.props')
    element(project, 'Import', Project='Recomp.props' if game == 'none' else r'..\Recomp.props')
    items = element(project, 'ItemGroup')
    filters = ET.Element('{' + NS + '}Project', ToolsVersion='Current')
    filter_items = element(filters, 'ItemGroup')
    groups = set()
    for source in source_files(root, game):
        relative = source.relative_to(root)
        include = '$(RecompRoot)' + str(relative).replace('/', '\\')
        tag = 'ClCompile' if source.suffix in {'.c', '.cc', '.cpp'} else 'ClInclude' if source.suffix in {'.h', '.hpp', '.inl'} else 'None'
        item = element(items, tag, Include=include)
        if tag == 'ClCompile' and ('debug' in relative.parts or 'release' in relative.parts):
            excluded = 'Release' if 'debug' in relative.parts else 'Debug'
            element(item, 'ExcludedFromBuild', 'true', Condition=f"'$(Configuration)'=='{excluded}'")
        category = 'Source Files' if tag == 'ClCompile' or source.suffix in {'.py', '.ps1', '.sh'} else 'Header Files' if tag == 'ClInclude' else 'Configuration'
        within = source.relative_to(root / 'games' / game) if game != 'none' else relative
        group = category if within.parent == Path('.') else category + '\\' + str(within.parent).replace('/', '\\')
        groups.add(group)
        entry = element(filter_items, tag, Include=include)
        element(entry, 'Filter', group)
    if game != 'none':
        for tag, pattern in [('ClCompile', 'cpu-*.c'), ('ClCompile', 'functions.c'), ('ClInclude', '*.h')]:
            include = f'$(RecompBuildDir)\\generated\\{game}\\{pattern}'
            attributes = {'Include': include}
            if pattern == 'functions.c':
                attributes['Condition'] = f"Exists('{include}')"
            element(items, tag, **attributes)
            entry = element(filter_items, tag, Include=include)
            element(entry, 'Filter', 'Generated Code')
        groups.add('Generated Code')
        references = element(project, 'ItemGroup')
        reference = element(references, 'ProjectReference', Include=r'..\recomp.vcxproj')
        element(reference, 'Project', guid('Recomp'))
        element(reference, 'ReferenceOutputAssembly', 'false')
        element(reference, 'LinkLibraryDependencies', 'false')
    definitions = element(filters, 'ItemGroup')
    expanded_groups = set()
    for group in groups:
        parts = group.split('\\')
        expanded_groups.update('\\'.join(parts[:i]) for i in range(1, len(parts) + 1))
    for group in sorted(expanded_groups):
        definition = element(definitions, 'Filter', Include=group)
        element(definition, 'UniqueIdentifier', guid(name + '/filter/' + group))
    element(project, 'Import', Project=r'$(VCTargetsPath)\Microsoft.Cpp.targets')
    write_xml(path, project)
    write_xml(path.with_suffix('.vcxproj.filters'), filters)
    return path


def write_solution(root: Path, selected: str = 'none') -> Path:
    games = sorted(p.parent.name for p in (root / 'games').glob('*/CMakeLists.txt'))
    if selected != 'none' and selected not in games:
        raise ValueError(f'Unknown game: {selected}')
    # F5 must be able to build any imported game chosen as the startup project.
    # Read each game's ROM location from its profile, keeping missing private
    # ROMs out of Build Solution without maintaining a second game/path list.
    enabled = {'Recomp'}
    for game in games:
        profile = root / 'games' / game / 'profile.toml'
        with profile.open('rb') as stream:
            rom = tomllib.load(stream)['game'].get('rom')
        if rom and (profile.parent / rom).is_file():
            enabled.add(game)
    if selected != 'none':
        enabled.add(selected)
    names = (['Recomp'] if selected == 'none' else [selected, 'Recomp']) + [game for game in games if game != selected]
    projects = {'Recomp': write_project(root, 'none')}
    projects.update((game, write_project(root, game)) for game in games)
    lines = ['Microsoft Visual Studio Solution File, Format Version 12.00', '# Visual Studio Version 17',
             '# Generated by tools/msvc_solution.py', 'VisualStudioVersion = 17.0.31903.59',
             'MinimumVisualStudioVersion = 10.0.40219.1']
    for name in names:
        relative = str(projects[name].relative_to(root)).replace('/', '\\')
        lines.append(f'Project("{CPP_PROJECT}") = "{name}", "{relative}", "{guid(name)}"')
        if name != 'Recomp':
            lines.extend(['\tProjectSection(ProjectDependencies) = postProject',
                          f'\t\t{guid("Recomp")} = {guid("Recomp")}', '\tEndProjectSection'])
        lines.append('EndProject')
    lines.extend([f'Project("{SOLUTION_FOLDER}") = "Games", "Games", "{guid("Games")}"', 'EndProject',
                  'Global', '\tGlobalSection(SolutionConfigurationPlatforms) = preSolution'])
    lines.extend(f'\t\t{mode}|x64 = {mode}|x64' for mode in ('Release', 'Debug'))
    lines.extend(['\tEndGlobalSection', '\tGlobalSection(ProjectConfigurationPlatforms) = postSolution'])
    for name in names:
        for mode in ('Release', 'Debug'):
            lines.append(f'\t\t{guid(name)}.{mode}|x64.ActiveCfg = {mode}|x64')
            if name in enabled:
                lines.append(f'\t\t{guid(name)}.{mode}|x64.Build.0 = {mode}|x64')
    lines.extend(['\tEndGlobalSection', '\tGlobalSection(NestedProjects) = preSolution'])
    lines.extend(f'\t\t{guid(game)} = {guid("Games")}' for game in games)
    lines.extend(['\tEndGlobalSection', 'EndGlobal'])
    output = root / 'v64recomp.sln'
    output.write_text('\n'.join(lines) + '\n', encoding='utf-8-sig', newline='\r\n')
    return output


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--game', default='none',
                        help='First/default game project; games with imported ROMs are always enabled for builds')
    args = parser.parse_args()
    print(f'Visual Studio solution: {write_solution(args.root.resolve(), args.game)}')
