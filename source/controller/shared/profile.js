// Creature: what the controllers show and the CC each control sends (channel 1).
// src/app.cpp binds the same numbers in kBind.
export const profile = {
  name: 'Creature',
  accent: '#8e3f7e',
  pages: [
    {key: 'P', label: 'PLAY', title: 'Play the creature', color: '#A6BD53', controls: [
      {id: 'wah', cc: 30, kind: 'trigger', label: 'WAH', key: 'A'},
      {id: 'roar', cc: 31, kind: 'trigger', label: 'ROAR', key: 'S'},
      {id: 'hush', cc: 32, kind: 'trigger', label: 'DRY', key: 'D'},
      {id: 'space', kind: 'xy', label: 'Growl × Mix', x: 'growl', y: 'mix'},
    ]},
    {key: 'V', label: 'VOICE', title: 'The voice', color: '#C77DBA', controls: [
      {id: 'voicing', cc: 21, kind: 'choice', label: 'Wah voicing', options: ['MOSS', 'VELVET', 'CINDER', 'REED'], def: 0},
      {id: 'growl', cc: 20, kind: 'range', label: 'Growl', min: 0, max: 100, unit: '%', def: 64},
      {id: 'mix', cc: 23, kind: 'range', label: 'Mix', min: 0, max: 100, unit: '%', def: 100},
      {id: 'level', cc: 24, kind: 'range', label: 'Level', min: 0, max: 100, unit: '%', def: 82},
      {id: 'dry', cc: 22, kind: 'toggle', label: 'Dry compare', def: 0},
    ]},
  ],
};
