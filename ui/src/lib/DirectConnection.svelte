<script lang="ts">
	import { Button, type ButtonProps } from '$lib/components/ui/button';
	import { formatDurationSec } from '$lib/formatDuration';
	import { getModeStyle, routeColor } from './modeStyle';
	import type { Itinerary, Leg } from '@motis-project/motis-client';

	const {
		d,
		...restProps
	}: {
		d: Itinerary;
	} & ButtonProps = $props();

	const legs = d.legs ?? [];
	const modeStyles = [
		...new Map(legs.map((l) => [JSON.stringify(getModeStyle(l)), getModeStyle(l)])).values()
	];

	const fallbackPlace = { name: 'UNKNOWN', lat: 0, lon: 0 } as const;
	const fallbackLeg: Leg = {
		mode: 'WALK',
		from: fallbackPlace,
		to: fallbackPlace,
		duration: d.duration,
		startTime: d.startTime,
		endTime: d.endTime,
		scheduledStartTime: d.startTime,
		scheduledEndTime: d.endTime,
		realTime: false,
		scheduled: false,
		legGeometry: { points: '', precision: 6, length: 0 }
	};

	const leg = legs.find((leg) => leg.mode !== 'WALK') ?? legs[0] ?? fallbackLeg;
</script>

<Button variant="child" {...restProps}>
	<div
		class="flex items-center py-1 px-2 rounded-lg font-bold text-sm h-8 text-nowrap"
		style={routeColor(leg)}
	>
		{#each modeStyles as [icon, _color, _textColor], i (i)}
			<svg class="relative mr-1 w-4 h-4 rounded-full">
				<use xlink:href={`#${icon}`}></use>
			</svg>
		{/each}
		{formatDurationSec(d.duration)}
	</div>
</Button>
