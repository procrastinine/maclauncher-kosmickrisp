// SPDX-License-Identifier: MIT
import {addLicense} from '../package-licences.mjs';
import {fileURLToPath} from 'node:url';
export function collectSteamacLicenses(root) {
  for (const [component, file] of [['steamac NOTICE', 'steamac-NOTICE.txt'], ['steamac Apache-2.0', 'steamac-LICENSE.txt'], ['steamac Mesa changes MIT', 'steamac-MIT.txt']])
    addLicense(root, component, fileURLToPath(new URL(file, import.meta.url)));
}
