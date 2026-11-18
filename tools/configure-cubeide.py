"""Configure the repository CubeIDE project; never copy application sources."""
import argparse
import copy
import json
import os
from pathlib import Path
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / 'firmware'
PREFIX = 'com.st.stm32cube.ide.mcu.gnu.managedbuild.'


def option(tool, suffix, value_type, value=None, items=None):
    superclass = PREFIX + suffix
    found = next((o for o in tool.findall('option') if o.get('superClass') == superclass), None)
    if found is None:
        found = ET.SubElement(tool, 'option', superClass=superclass)
    found.set('valueType', value_type)
    found.set('useByScannerDiscovery', 'false')
    if value is not None:
        found.set('value', value)
    for child in list(found):
        found.remove(child)
    if items is not None:
        found.attrib.pop('value', None)
        found.set('IS_BUILTIN_EMPTY', 'false')
        found.set('IS_VALUE_EMPTY', 'false')
        for item in items:
            ET.SubElement(found, 'listOptionValue', builtIn='false', value=item)
    return found


def eclipse_uri(path):
    relative = Path(os.path.relpath(path, FW))
    if relative.drive:
        raise ValueError('Dependency must be on the same drive for portable Eclipse links')
    parts = list(relative.parts)
    count = 0
    while parts and parts[0] == '..':
        count += 1
        parts.pop(0)
    base = f'PARENT-{count}-PROJECT_LOC' if count else 'PROJECT_LOC'
    return base + '/' + '/'.join(parts)


def save_xml(path, root, file_version=False):
    ET.indent(root, space='\t')
    declaration = '<?xml version="1.0" encoding="UTF-8"?>\n'
    if file_version:
        declaration += '<?fileVersion 4.0.0?>\n'
    path.write_text(declaration + ET.tostring(root, encoding='unicode') + '\n', encoding='utf-8')


def debug_launch(profile, config_id):
    root = ET.Element('launchConfiguration', type='com.st.stm32cube.ide.mcu.debug.launch.launchConfigurationType')
    strings = {
        'org.eclipse.cdt.launch.PROJECT_ATTR': 'moto-nav-hud',
        'org.eclipse.cdt.launch.PROGRAM_NAME': profile + '/moto-nav-hud.elf',
        'org.eclipse.cdt.launch.PROJECT_BUILD_CONFIG_ID_ATTR': config_id,
        'org.eclipse.cdt.dsf.gdb.DEBUG_NAME': 'arm-none-eabi-gdb',
        'org.eclipse.cdt.debug.gdbjtag.core.jtagDeviceId': 'com.st.stm32cube.ide.mcu.debug.stlink',
        'org.eclipse.cdt.debug.gdbjtag.core.ipAddress': 'localhost',
        'org.eclipse.cdt.debug.gdbjtag.core.stopAt': 'main',
        'org.eclipse.cdt.debug.gdbjtag.core.initCommands':
            'set *(unsigned int *)0xE0042008 = (*(unsigned int *)0xE0042008 | 0x1000)',
        'com.st.stm32cube.ide.mcu.debug.stlink.reset_strategy': 'connect_under_reset',
        'com.st.stm32cube.ide.mcu.debug.stlink.watchdog_config': 'none',
        'com.st.stm32cube.ide.mcu.debug.launch.swd_mode': 'true',
        'com.st.stm32cube.ide.mcu.debug.launch.swv_trace_hclk': '180000000',
        'com.st.stm32cube.ide.mcu.debug.launch.loadList': json.dumps({'fItems': [dict(
            fIsFromMainTab=True, fPath=profile + '/moto-nav-hud.elf', fProjectName='moto-nav-hud',
            fPerformBuild=True, fDownload=True, fLoadSymbols=True)]}, separators=(',', ':')),
    }
    for key, value in strings.items():
        ET.SubElement(root, 'stringAttribute', key=key, value=value)
    for key, value in {
        'org.eclipse.cdt.launch.PROJECT_BUILD_CONFIG_AUTO_ATTR': False,
        'org.eclipse.cdt.debug.gdbjtag.core.setResume': True,
        'org.eclipse.cdt.debug.gdbjtag.core.setStopAt': True,
        'com.st.stm32cube.ide.mcu.debug.launch.startServer': True,
        'com.st.stm32cube.ide.mcu.debug.launch.useRemoteTarget': True,
        'com.st.stm32cube.ide.mcu.debug.launch.verify_flash_download': True,
    }.items():
        ET.SubElement(root, 'booleanAttribute', key=key, value=str(value).lower())
    ET.SubElement(root, 'intAttribute', key='org.eclipse.cdt.debug.gdbjtag.core.portNumber', value='61234')
    mapped = ET.SubElement(root, 'listAttribute', key='org.eclipse.debug.core.MAPPED_RESOURCE_PATHS')
    ET.SubElement(mapped, 'listEntry', value='/moto-nav-hud')
    mapped = ET.SubElement(root, 'listAttribute', key='org.eclipse.debug.core.MAPPED_RESOURCE_TYPES')
    ET.SubElement(mapped, 'listEntry', value='4')
    save_xml(FW / ('moto-nav-hud ' + profile + '.launch'), root)


def configure(dependencies, freertos):
    manifest = json.loads((ROOT / 'tools/firmware-sources.json').read_text(encoding='utf-8'))
    links = dict(ExternalHAL=dependencies / 'Drivers/STM32F4xx_HAL_Driver',
                 ExternalCMSIS=dependencies / 'Drivers/CMSIS',
                 ExternalLVGL=dependencies / 'lvgl', ExternalFreeRTOS=freertos)
    required = [links['ExternalLVGL'] / 'lvgl.h', links['ExternalCMSIS'] / 'Core/Include/core_cm4.h']
    required += [links['ExternalHAL'] / 'Src' / name for name in manifest['hal']]
    required += [freertos / name for name in manifest['freertos']]
    for path in required:
        if not path.is_file():
            raise FileNotFoundError(path)

    project = ET.parse(FW / '.project').getroot()
    project.find('name').text = 'moto-nav-hud'
    linked = project.find('linkedResources')
    if linked is not None:
        project.remove(linked)
    linked = ET.SubElement(project, 'linkedResources')
    for name, path in links.items():
        link = ET.SubElement(linked, 'link')
        ET.SubElement(link, 'name').text = name
        ET.SubElement(link, 'type').text = '2'
        ET.SubElement(link, 'locationURI').text = eclipse_uri(path)

    root = ET.parse(FW / '.cproject').getroot()
    settings = root.find("./storageModule[@moduleId='org.eclipse.cdt.core.settings']")
    previous = settings.findall('cconfiguration')
    templates = {}
    for c in previous:
        name = c.find('./storageModule').get('name')
        templates.setdefault(name.split('-')[0], c)
    for c in previous:
        settings.remove(c)
    includes = ['../Inc', '..'] + ['"${workspace_loc:/${ProjName}/' + path + '}"' for path in (
        'ExternalLVGL', 'ExternalHAL/Inc', 'ExternalCMSIS/Device/ST/STM32F4xx/Include',
        'ExternalCMSIS/Core/Include', 'ExternalFreeRTOS/include', 'ExternalFreeRTOS/portable/GCC/ARM_CM4F')]
    hal_excluded = ['Legacy'] + sorted(p.name for p in (links['ExternalHAL'] / 'Src').glob('*.c')
                                     if p.name not in manifest['hal'])
    rtos_excluded = ['portable', 'examples'] + sorted(p.name for p in freertos.glob('*.c')
                                                    if p.name not in manifest['freertos'])
    profiles = [('Debug-BLE', True), ('Debug-VCP', False), ('Release-BLE', True), ('Release-VCP', False)]
    for number, (name, ble) in enumerate(profiles):
        debug = name.startswith('Debug')
        c = copy.deepcopy(templates['Debug' if debug else 'Release'])
        config_id = PREFIX + f'config.exe.{"debug" if debug else "release"}.{1800000000 + number}'
        settings.append(c)
        config = c.find("./storageModule[@moduleId='cdtBuildSystem']/configuration")
        c.set('id', config_id)
        core = c.find("./storageModule[@moduleId='org.eclipse.cdt.core.settings']")
        core.set('id', config_id)
        core.set('name', name)
        config.set('id', config_id)
        config.set('name', name)
        config.set('artifactName', '${ProjName}')
        chain = config.find('./folderInfo/toolChain')
        chain.find('builder').set('buildPath', '${workspace_loc:/${ProjName}}/' + name)
        chain.find('builder').set('parallelizationNumber', '4')
        # Stable IDs make this generator idempotent and remove duplicate old configurations.
        for index, element in enumerate(c.iter()):
            if 'id' in element.attrib and element not in (c, core, config):
                element.set('id', config_id + '.' + element.tag + '.' + str(index))
        config.find('folderInfo').set('id', config_id + '.')
        defaults = chain.find("option[@superClass='" + PREFIX + "option.defaults']")
        if defaults is not None:
            chain.remove(defaults)  # Old encoded defaults describe the obsolete bare-metal project.
        compiler = chain.find("tool[@superClass='" + PREFIX + "tool.c.compiler']")
        defines = ['STM32F446xx', 'USE_HAL_DRIVER', 'LV_CONF_INCLUDE_SIMPLE', 'NAV_USE_BLE=' + str(int(ble))]
        if debug:
            defines.append('DEBUG')
        option(compiler, 'tool.c.compiler.option.definedsymbols', 'definedSymbols', items=defines)
        option(compiler, 'tool.c.compiler.option.includepaths', 'includePath', items=includes)
        option(compiler, 'tool.c.compiler.option.optimization.level', 'enumerated',
               PREFIX + 'tool.c.compiler.option.optimization.level.value.' + ('og' if debug else 'os'))
        linker = chain.find("tool[@superClass='" + PREFIX + "tool.c.linker']")
        option(linker, 'tool.c.linker.option.script', 'string', '${workspace_loc:/${ProjName}/STM32F446RETX_FLASH.ld}')
        sources = config.find('sourceEntries')
        sources.clear()
        entries = [('Src', ''), ('Startup', ''), ('Inc', ''),
                   ('ExternalHAL/Src', '|'.join(hal_excluded)), ('ExternalLVGL/src', ''),
                   ('ExternalFreeRTOS', '|'.join(rtos_excluded)),
                   ('ExternalFreeRTOS/portable/GCC/ARM_CM4F', '')]
        for path, excluded in entries:
            attrs = dict(flags='VALUE_WORKSPACE_PATH|RESOLVED', kind='sourcePath', name=path)
            if excluded:
                attrs['excluding'] = excluded
            ET.SubElement(sources, 'entry', attrs)
        if debug:
            debug_launch(name, config_id)
    scanner = root.find("storageModule[@moduleId='scannerConfiguration']")
    if scanner is not None:
        for child in list(scanner):
            if child.tag == 'scannerConfigBuildInfo':
                scanner.remove(child)
    project_info = root.find("./storageModule[@moduleId='cdtBuildSystem']/project")
    project_info.set('name', 'moto-nav-hud')
    project_info.set('id', 'moto-nav-hud.project')
    save_xml(FW / '.project', project)
    save_xml(FW / '.cproject', root, file_version=True)
    print('CubeIDE project moto-nav-hud: ' + ', '.join(name for name, _ in profiles))
    for name, path in links.items():
        print(f'  {name}: {path}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependencies-root', type=Path, default=ROOT.parent / 'ws/moto-nav-hud-fw')
    parser.add_argument('--freertos-root', type=Path, default=ROOT.parent / 'deps/FreeRTOS-Kernel')
    args = parser.parse_args()
    configure(args.dependencies_root.resolve(), args.freertos_root.resolve())
